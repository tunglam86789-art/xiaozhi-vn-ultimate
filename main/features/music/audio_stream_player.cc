/**
 * @file audio_stream_player.cc
 * @brief Implementation of the base HTTP audio stream player.
 *
 * Common logic shared between Esp32Music and Esp32Radio:
 *   - HTTP download with reconnect (FreeRTOS task, pinned core)
 *   - Producer-consumer audio buffer in PSRAM
 *   - esp_audio_codec simple decoder (MP3 / AAC / auto)
 *   - Mono down-mix, volume amplification
 *   - FFT display feeding
 */

#include "audio_stream_player.h"
#include "board.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstring>
#include <algorithm>
#include <cmath>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "AudioStreamPlayer";

/* ================================================================== */
/*  Constructor / Destructor                                          */
/* ================================================================== */

AudioStreamPlayer::AudioStreamPlayer()
    : is_playing_(false),
      is_downloading_(false),
      display_mode_(DISPLAY_MODE_SPECTRUM),
      volume_factor_(AUDIO_DEFAULT_VOLUME),
      download_task_handle_(nullptr),
      play_task_handle_(nullptr),
      buffer_mutex_(nullptr),
      buffer_data_sem_(nullptr),
      buffer_space_sem_(nullptr),
      buffer_size_(0),
      decoder_(nullptr),
      dec_info_{},
      decoder_initialized_(false),
      dec_info_ready_(false),
      decoder_type_(AudioDecoderType::MP3),
      pcm_out_buffer_(nullptr),
      pcm_out_buffer_size_(0),
      input_buffer_(nullptr),
      input_bytes_left_(0),
      current_play_time_ms_(0),
      total_frames_decoded_(0),
      fft_started_(false),
      info_displayed_(false),
      fft_pcm_ptr_(nullptr)
{
    buffer_mutex_     = xSemaphoreCreateMutex();
    buffer_data_sem_  = xSemaphoreCreateCounting(128, 0);
    buffer_space_sem_ = xSemaphoreCreateCounting(128, 128);
}

AudioStreamPlayer::~AudioStreamPlayer()
{
    ESP_LOGI(TAG, "Destroying AudioStreamPlayer - stopping all operations");
    StopStream();

    if (buffer_mutex_)     vSemaphoreDelete(buffer_mutex_);
    if (buffer_data_sem_)  vSemaphoreDelete(buffer_data_sem_);
    if (buffer_space_sem_) vSemaphoreDelete(buffer_space_sem_);
}

/* ================================================================== */
/*  Public: StartStream / StopStream                                  */
/* ================================================================== */

bool AudioStreamPlayer::StartStream(const std::string& url, AudioDecoderType type)
{
    if (url.empty()) {
        ESP_LOGE(TAG, "Stream URL is empty");
        return false;
    }

    ESP_LOGI(TAG, "StartStream: url=%s, decoder=%d", url.c_str(), (int)type);

    /* Stop previous session */
    StopStream();

    /* Reset FFT / display on the current display */
    {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->StopFFT();
            display->ReleaseAudioBuffFFT();
            display->SetMusicInfo(nullptr);
            ESP_LOGI(TAG, "Display memory released before starting stream");
        }
    }

    stream_url_          = url;
    decoder_type_        = type;
    fft_started_         = false;
    info_displayed_      = false;
    current_play_time_ms_ = 0;
    total_frames_decoded_ = 0;
    buffer_size_          = 0;

    /* Clear buffer */
    ClearAudioBuffer();

    /* Launch download task */
    is_downloading_ = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        DownloadTaskEntry, "stream_dl",
        AUDIO_DOWNLOAD_TASK_STACK, this,
        AUDIO_DOWNLOAD_TASK_PRIO, &download_task_handle_,
        AUDIO_DOWNLOAD_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create download task");
        is_downloading_ = false;
        return false;
    }

    /* Launch playback task */
    is_playing_ = true;
    ret = xTaskCreatePinnedToCore(
        PlayTaskEntry, "stream_play",
        AUDIO_PLAY_TASK_STACK, this,
        AUDIO_PLAY_TASK_PRIO, &play_task_handle_,
        AUDIO_PLAY_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        is_playing_ = false;
        is_downloading_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Streaming tasks started successfully");
    return true;
}

bool AudioStreamPlayer::StopStream()
{
    if (!is_playing_ && !is_downloading_) {
        return true;
    }

    ESP_LOGI(TAG, "StopStream: stopping download=%d, playing=%d",
             is_downloading_.load(), is_playing_.load());

    ResetSampleRate();

    is_downloading_ = false;
    is_playing_     = false;

    /* Signal semaphores so tasks can wake up and exit */
    if (buffer_data_sem_)  xSemaphoreGive(buffer_data_sem_);
    if (buffer_space_sem_) xSemaphoreGive(buffer_space_sem_);

    /* Wait for tasks to finish (with timeout) */
    const TickType_t timeout = pdMS_TO_TICKS(5000);

    if (download_task_handle_) {
        /* eTaskGetState returns eDeleted once the task has been cleaned up */
        for (int i = 0; i < 50 && download_task_handle_ && eTaskGetState(download_task_handle_) != eDeleted; ++i) {
            xSemaphoreGive(buffer_space_sem_);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        download_task_handle_ = nullptr;
        ESP_LOGI(TAG, "Download task finished");
    }

    if (play_task_handle_) {
        for (int i = 0; i < 50 && play_task_handle_ && eTaskGetState(play_task_handle_) != eDeleted; ++i) {
            xSemaphoreGive(buffer_data_sem_);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        play_task_handle_ = nullptr;
        ESP_LOGI(TAG, "Play task finished");
    }

    /* Clear display */
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetMusicInfo("");
        if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
            display->StopFFT();
            display->ReleaseAudioBuffFFT();
        }
    }

    fft_pcm_ptr_ = nullptr;
    fft_started_ = false;

    ClearAudioBuffer();
    CleanupDecoder();

    OnPlaybackFinished();

    ESP_LOGI(TAG, "Stream stopped and cleaned up");
    return true;
}

void AudioStreamPlayer::SetDisplayMode(DisplayMode mode)
{
    DisplayMode old = display_mode_.load();
    display_mode_ = mode;
    ESP_LOGI(TAG, "Display mode: %d -> %d", (int)old, (int)mode);
}

/* ================================================================== */
/*  FreeRTOS task entry points                                        */
/* ================================================================== */

void AudioStreamPlayer::DownloadTaskEntry(void* param)
{
    auto* self = static_cast<AudioStreamPlayer*>(param);
    self->DownloadLoop(self->stream_url_);
    self->download_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void AudioStreamPlayer::PlayTaskEntry(void* param)
{
    auto* self = static_cast<AudioStreamPlayer*>(param);
    self->PlayLoop();
    self->play_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

/* ================================================================== */
/*  Download loop                                                     */
/* ================================================================== */

void AudioStreamPlayer::DownloadLoop(const std::string& url)
{
    ESP_LOGI(TAG, "Download task started: %s", url.c_str());

    if (url.empty() || url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL: %s", url.c_str());
        is_downloading_ = false;
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-");

    /* Let subclass add custom headers (e.g. auth) */
    OnPrepareHttp(http.get());

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to connect: %s", url.c_str());
        is_downloading_ = false;
        return;
    }

    int status = http->GetStatusCode();
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "HTTP status %d for URL: %s", status, url.c_str());
        http->Close();
        is_downloading_ = false;
        return;
    }

    ESP_LOGI(TAG, "Connected, HTTP status=%d", status);

    char* buf = new (std::nothrow) char[AUDIO_HTTP_CHUNK_SIZE];
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        http->Close();
        is_downloading_ = false;
        return;
    }

    size_t total_downloaded = 0;
    size_t log_counter      = 0;
    int    reconnect_tries  = 0;

    while (is_downloading_ && is_playing_) {
        int bytes_read = http->Read(buf, AUDIO_HTTP_CHUNK_SIZE);

        /* ---- Handle errors / reconnect ---- */
        if (bytes_read <= 0) {
            if (bytes_read == 0 && reconnect_tries == 0) {
                /* Natural end of finite stream (e.g. file download) */
                ESP_LOGI(TAG, "Stream ended after %zu bytes", total_downloaded);
                break;
            }

            reconnect_tries++;
            ESP_LOGW(TAG, "Stream read failed (%d), reconnect %d/%d",
                     bytes_read, reconnect_tries, AUDIO_MAX_RECONNECT);

            if (reconnect_tries > AUDIO_MAX_RECONNECT) {
                ESP_LOGE(TAG, "Max reconnect attempts exceeded");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(AUDIO_RECONNECT_DELAY_MS));
            http->Close();

            if (!http->Open("GET", url)) {
                ESP_LOGE(TAG, "Reconnect failed at attempt %d", reconnect_tries);
                continue;
            }
            OnPrepareHttp(http.get());
            ESP_LOGI(TAG, "Reconnected at attempt %d", reconnect_tries);
            continue;
        }

        reconnect_tries = 0;

        /* ---- Detect format on first data ---- */
        if (total_downloaded == 0 && bytes_read >= 4) {
            const uint8_t* d = (const uint8_t*)buf;
            if (memcmp(d, "ID3", 3) == 0) {
                ESP_LOGI(TAG, "Detected MP3 with ID3 tag");
            } else if ((d[0] & 0xFF) == 0xFF && (d[1] & 0xE0) == 0xE0) {
                ESP_LOGI(TAG, "Detected raw MP3 frame");
            } else {
                ESP_LOGI(TAG, "Stream header: %02X %02X %02X %02X",
                         d[0], d[1], d[2], d[3]);
            }
        }

        /* ---- Allocate PSRAM chunk ---- */
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "PSRAM alloc failed for %d bytes", bytes_read);
            break;
        }
        memcpy(chunk_data, buf, bytes_read);

        /* ---- Push to buffer with back-pressure ---- */
        /* Wait until there is space */
        while (is_downloading_ && buffer_size_ >= AUDIO_BUF_MAX_SIZE) {
            xSemaphoreTake(buffer_space_sem_, pdMS_TO_TICKS(100));
        }

        if (!is_downloading_) {
            heap_caps_free(chunk_data);
            break;
        }

        if (xSemaphoreTake(buffer_mutex_, pdMS_TO_TICKS(500)) == pdTRUE) {
            audio_buffer_.push(StreamAudioChunk(chunk_data, bytes_read));
            buffer_size_ += bytes_read;
            total_downloaded += bytes_read;
            log_counter += bytes_read;
            xSemaphoreGive(buffer_mutex_);

            /* Signal playback task */
            xSemaphoreGive(buffer_data_sem_);

            if (log_counter >= AUDIO_LOG_INTERVAL) {
                log_counter = 0;
                ESP_LOGI(TAG, "Downloaded %zu bytes, buffer=%zu", total_downloaded, buffer_size_);
            }
        } else {
            heap_caps_free(chunk_data);
            ESP_LOGW(TAG, "Buffer mutex timeout");
        }
    }

    delete[] buf;
    http->Close();
    is_downloading_ = false;

    /* Wake up playback task */
    xSemaphoreGive(buffer_data_sem_);

    ESP_LOGI(TAG, "Download task finished. Total: %zu bytes", total_downloaded);
}

/* ================================================================== */
/*  Playback loop                                                     */
/* ================================================================== */

void AudioStreamPlayer::PlayLoop()
{
    ESP_LOGI(TAG, "Playback task started");

    current_play_time_ms_ = 0;
    total_frames_decoded_ = 0;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "Audio codec not available");
        is_playing_ = false;
        return;
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    /* Wait until minimum buffer is filled */
    while (is_playing_ && buffer_size_ < AUDIO_BUF_MIN_SIZE && is_downloading_) {
        xSemaphoreTake(buffer_data_sem_, pdMS_TO_TICKS(100));
    }

    /* Initialize decoder */
    if (!InitDecoder(decoder_type_)) {
        ESP_LOGE(TAG, "Decoder init failed");
        is_playing_ = false;
        return;
    }

    ESP_LOGI(TAG, "Playback starting, buffer=%zu", buffer_size_);

    /* Allocate decoder input buffer in PSRAM */
    input_buffer_ = (uint8_t*)heap_caps_malloc(AUDIO_DEC_INPUT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!input_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate decoder input buffer");
        is_playing_ = false;
        return;
    }
    input_bytes_left_ = 0;

    auto& app      = Application::GetInstance();
    auto  display   = Board::GetInstance().GetDisplay();
    size_t log_counter = 0;
    size_t total_played = 0;

    while (is_playing_) {
        /* ---- Check device state ---- */
        DeviceState ds = app.GetDeviceState();
        if (ds == kDeviceStateListening || ds == kDeviceStateSpeaking) {
            app.ToggleChatState();
            vTaskDelay(pdMS_TO_TICKS(AUDIO_CHAT_TOGGLE_DELAY_MS));
            continue;
        } else if (ds != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_STATE_POLL_MS));
            continue;
        }

        /* ---- FFT start (once) ---- */
        if (!fft_started_) {
            if (display && display_mode_ == DISPLAY_MODE_SPECTRUM) {
                vTaskDelay(pdMS_TO_TICKS(150));
                display->StartFFT();
                ESP_LOGI(TAG, "FFT started");
            }
            OnDisplayReady();
            fft_started_ = true;
        }

        /* ---- Fill decoder input buffer ---- */
        if (input_bytes_left_ < (AUDIO_DEC_INPUT_BUF_SIZE / 2)) {
            StreamAudioChunk chunk;
            bool got_chunk = false;

            if (xSemaphoreTake(buffer_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (!audio_buffer_.empty()) {
                    chunk = audio_buffer_.front();
                    audio_buffer_.pop();
                    buffer_size_ -= chunk.size;
                    got_chunk = true;
                }
                xSemaphoreGive(buffer_mutex_);
                xSemaphoreGive(buffer_space_sem_);  /* signal download task */
            }

            if (!got_chunk) {
                if (!is_downloading_ && buffer_size_ == 0) {
                    ESP_LOGI(TAG, "Stream ended, total played=%zu", total_played);
                    break;
                }
                xSemaphoreTake(buffer_data_sem_, pdMS_TO_TICKS(50));
                continue;
            }

            if (chunk.data && chunk.size > 0) {
                size_t space = AUDIO_DEC_INPUT_BUF_SIZE - input_bytes_left_;
                size_t copy  = std::min(chunk.size, space);
                memcpy(input_buffer_ + input_bytes_left_, chunk.data, copy);
                input_bytes_left_ += copy;
                total_played += chunk.size;
                log_counter  += chunk.size;
                heap_caps_free(chunk.data);
            }
        }

        if (input_bytes_left_ <= 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /* ---- Decode ---- */
        bool input_eos = (!is_downloading_ && buffer_size_ == 0);

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = input_buffer_;
        raw.len    = (uint32_t)input_bytes_left_;
        raw.eos    = input_eos;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;

        esp_audio_simple_dec_out_t out = {};

        /* Use resizeable vector for AAC, fixed buffer for MP3 */
        if (decoder_type_ == AudioDecoderType::AAC) {
            if (dec_out_vec_.empty()) dec_out_vec_.resize(AUDIO_PCM_OUT_BUF_SIZE);
            out.buffer = dec_out_vec_.data();
            out.len    = dec_out_vec_.size();
        } else {
            out.buffer       = pcm_out_buffer_;
            out.len          = (uint32_t)pcm_out_buffer_size_;
        }
        out.decoded_size = 0;
        out.needed_size  = 0;

        esp_audio_err_t dec_ret = esp_audio_simple_dec_process(decoder_, &raw, &out);

        if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (decoder_type_ == AudioDecoderType::AAC) {
                dec_out_vec_.resize(out.needed_size);
                out.buffer = dec_out_vec_.data();
                out.len    = out.needed_size;
            } else {
                ESP_LOGI(TAG, "PCM buffer too small, need %u, reallocating", out.needed_size);
                heap_caps_free(pcm_out_buffer_);
                pcm_out_buffer_size_ = out.needed_size;
                pcm_out_buffer_ = (uint8_t*)heap_caps_malloc(pcm_out_buffer_size_, MALLOC_CAP_SPIRAM);
                if (!pcm_out_buffer_) {
                    ESP_LOGE(TAG, "PCM buffer realloc failed");
                    break;
                }
            }
            continue;  /* retry decode */
        }

        /* Advance input past consumed bytes */
        if (raw.consumed > 0) {
            input_bytes_left_ -= raw.consumed;
            if (input_bytes_left_ > 0) {
                memmove(input_buffer_, input_buffer_ + raw.consumed, input_bytes_left_);
            }
        }

        if (dec_ret == ESP_AUDIO_ERR_DATA_LACK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (dec_ret == ESP_AUDIO_ERR_CONTINUE) {
            continue;
        }
        if (dec_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Decode error %d, skipping byte", dec_ret);
            if (input_bytes_left_ > 0) {
                input_bytes_left_--;
                memmove(input_buffer_, input_buffer_ + 1, input_bytes_left_);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ---- Successful decode ---- */
        esp_audio_simple_dec_get_info(decoder_, &dec_info_);

        /* Notify subclass once */
        if (!dec_info_ready_ && dec_info_.sample_rate > 0) {
            dec_info_ready_ = true;
            ESP_LOGI(TAG, "Stream: %d Hz, %d bit, %d ch",
                     dec_info_.sample_rate, dec_info_.bits_per_sample, dec_info_.channel);
            OnStreamInfoReady(dec_info_.sample_rate, dec_info_.bits_per_sample, dec_info_.channel);
        }

        total_frames_decoded_++;

        if (dec_info_.sample_rate == 0 || dec_info_.channel == 0) {
            continue;
        }

        /* ---- Calculate play time ---- */
        int bits   = (dec_info_.bits_per_sample > 0) ? dec_info_.bits_per_sample : 16;
        int chans  = (dec_info_.channel > 0) ? dec_info_.channel : 1;
        int bps    = bits / 8;
        int total_samples      = out.decoded_size / bps;
        int samples_per_chan   = total_samples / chans;
        int frame_duration_ms  = (samples_per_chan * 1000) / dec_info_.sample_rate;
        current_play_time_ms_ += frame_duration_ms;

        /* Hook for subclass */
        OnPcmFrame(current_play_time_ms_, dec_info_.sample_rate, chans);

        /* ---- Mono downmix ---- */
        int16_t* pcm_in = reinterpret_cast<int16_t*>(out.buffer);
        std::vector<int16_t> mono_buf;
        int16_t* final_pcm   = pcm_in;
        int      final_count = total_samples;

        if (chans == 2) {
            mono_buf.resize(samples_per_chan);
            for (int i = 0; i < samples_per_chan; ++i) {
                int l = pcm_in[i * 2];
                int r = pcm_in[i * 2 + 1];
                mono_buf[i] = (int16_t)((l + r) / 2);
            }
            final_pcm   = mono_buf.data();
            final_count = samples_per_chan;
        } else if (chans == 1) {
            final_count = total_samples;
        }

        /* ---- Volume amplification ---- */
        std::vector<int16_t> amp_buf(final_count);
        for (int i = 0; i < final_count; ++i) {
            int32_t s = (int32_t)(final_pcm[i] * volume_factor_);
            if (s > INT16_MAX)      s = INT16_MAX;
            else if (s < INT16_MIN) s = INT16_MIN;
            amp_buf[i] = (int16_t)s;
        }

        /* ---- Build packet ---- */
        AudioStreamPacket pkt;
        pkt.sample_rate     = dec_info_.sample_rate;
        pkt.frame_duration  = 60;
        pkt.timestamp       = 0;

        size_t pcm_bytes = final_count * sizeof(int16_t);
        pkt.payload.resize(pcm_bytes);
        memcpy(pkt.payload.data(), amp_buf.data(), pcm_bytes);

        /* ---- Feed FFT ---- */
        if (display && display_mode_ == DISPLAY_MODE_SPECTRUM) {
            fft_pcm_ptr_ = display->MakeAudioBuffFFT(pcm_bytes);
            display->FeedAudioDataFFT(amp_buf.data(), pcm_bytes);
        }

        /* ---- Send to audio output ---- */
        app.AddAudioData(std::move(pkt));

        /* ---- Log progress ---- */
        if (log_counter >= AUDIO_LOG_INTERVAL) {
            log_counter = 0;
            ESP_LOGI(TAG, "Played %zu bytes, buffer=%zu", total_played, buffer_size_);
        }

        /* ---- EOS check ---- */
        if (input_eos && input_bytes_left_ == 0) {
            ESP_LOGI(TAG, "End of stream");
            break;
        }
    }

    /* ---- Cleanup ---- */
    if (input_buffer_) {
        heap_caps_free(input_buffer_);
        input_buffer_ = nullptr;
    }
    input_bytes_left_ = 0;

    if (is_playing_) {
        ClearAudioBuffer();
        ResetSampleRate();
    }

    is_playing_ = false;

    if (display && display_mode_ == DISPLAY_MODE_SPECTRUM) {
        display->SetMusicInfo("");
        display->StopFFT();
        display->ReleaseAudioBuffFFT();
    }

    fft_pcm_ptr_ = nullptr;
    CleanupDecoder();

    /* Re-enable output for next user */
    auto codec2 = Board::GetInstance().GetAudioCodec();
    if (codec2) codec2->EnableOutput(true);

    ESP_LOGI(TAG, "Playback task finished. Total played=%zu", total_played);
}

/* ================================================================== */
/*  Buffer helpers                                                    */
/* ================================================================== */

void AudioStreamPlayer::ClearAudioBuffer()
{
    if (xSemaphoreTake(buffer_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
        while (!audio_buffer_.empty()) {
            auto chunk = audio_buffer_.front();
            audio_buffer_.pop();
            if (chunk.data) heap_caps_free(chunk.data);
        }
        buffer_size_ = 0;
        xSemaphoreGive(buffer_mutex_);
    }
    ESP_LOGI(TAG, "Audio buffer cleared");
}

/* ================================================================== */
/*  Decoder helpers                                                   */
/* ================================================================== */

bool AudioStreamPlayer::InitDecoder(AudioDecoderType type)
{
    if (decoder_initialized_) {
        ESP_LOGW(TAG, "Decoder already initialised");
        return true;
    }

    ESP_LOGI(TAG, "Initialising decoder type=%d", (int)type);

    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    esp_audio_simple_dec_cfg_t cfg = {};
    if (type == AudioDecoderType::AAC) {
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    } else {
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    cfg.dec_cfg  = nullptr;
    cfg.cfg_size = 0;

    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &decoder_);
    if (ret != ESP_AUDIO_ERR_OK || !decoder_) {
        ESP_LOGE(TAG, "Decoder open failed: %d", ret);
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        return false;
    }

    /* Allocate PCM output buffer */
    pcm_out_buffer_size_ = AUDIO_PCM_OUT_BUF_SIZE;
    pcm_out_buffer_ = (uint8_t*)heap_caps_malloc(pcm_out_buffer_size_, MALLOC_CAP_SPIRAM);
    if (!pcm_out_buffer_) {
        ESP_LOGE(TAG, "PCM buffer alloc failed");
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
        return false;
    }

    if (type == AudioDecoderType::AAC) {
        dec_out_vec_.resize(AUDIO_PCM_OUT_BUF_SIZE);
    }

    memset(&dec_info_, 0, sizeof(dec_info_));
    dec_info_ready_      = false;
    decoder_initialized_ = true;
    decoder_type_        = type;

    ESP_LOGI(TAG, "Decoder initialised (type=%d)", (int)type);
    return true;
}

void AudioStreamPlayer::CleanupDecoder()
{
    if (!decoder_initialized_) return;

    if (decoder_) {
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
    }
    if (pcm_out_buffer_) {
        heap_caps_free(pcm_out_buffer_);
        pcm_out_buffer_ = nullptr;
        pcm_out_buffer_size_ = 0;
    }

    dec_out_vec_.clear();
    dec_info_ready_      = false;
    decoder_initialized_ = false;

    esp_audio_simple_dec_unregister_default();
    esp_audio_dec_unregister_default();

    ESP_LOGI(TAG, "Decoder cleaned up");
}

AudioDecoderType AudioStreamPlayer::DetectStreamType(const uint8_t* data, size_t len)
{
    if (!data || len < 4) return AudioDecoderType::MP3;

    /* ID3 tag => MP3 */
    if (memcmp(data, "ID3", 3) == 0) return AudioDecoderType::MP3;

    /* MP3 sync word */
    if ((data[0] & 0xFF) == 0xFF && (data[1] & 0xE0) == 0xE0) return AudioDecoderType::MP3;

    /* ADTS AAC sync */
    if ((data[0] & 0xFF) == 0xFF && (data[1] & 0xF0) == 0xF0) return AudioDecoderType::AAC;

    return AudioDecoderType::MP3;  /* default */
}

/* ================================================================== */
/*  Sample rate reset                                                 */
/* ================================================================== */

void AudioStreamPlayer::ResetSampleRate()
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 &&
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "Resetting sample rate: %d -> %d Hz",
                 codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) {
            ESP_LOGI(TAG, "Sample rate reset to %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "Failed to reset sample rate");
        }
    }
}
