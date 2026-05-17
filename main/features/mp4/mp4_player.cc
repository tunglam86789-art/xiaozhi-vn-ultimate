#include "mp4_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

extern "C" {
#include "esp_extractor_defaults.h"
#include "extractor_helper.h"
}

#include "audio_codec.h"
#include "board.h"
#include "sd_card.h"

static const char* TAG = "Mp4Player";

namespace {
constexpr size_t kAudioPcmBufferSize = MP4_AUDIO_BUF_SIZE;

inline int16_t ClampToInt16(float value) {
    if (value > 32767.0f) return 32767;
    if (value < -32768.0f) return -32768;
    return static_cast<int16_t>(value);
}
}  // namespace

Mp4Player& Mp4Player::GetInstance() {
    static Mp4Player instance;
    return instance;
}

Mp4Player::Mp4Player() = default;

Mp4Player::~Mp4Player() {
    Deinitialize();
}

bool Mp4Player::Initialize(esp_lcd_panel_handle_t lcd_panel,
                           uint16_t lcd_width, uint16_t lcd_height,
                           AudioCodec* codec,
                           SdCard* sd_card) {
    if (initialized_.load()) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }
    if (!lcd_panel || !codec || !sd_card) {
        ESP_LOGE(TAG, "Invalid init args: panel=%p codec=%p sd=%p",
                 lcd_panel, codec, sd_card);
        return false;
    }

    lcd_panel_ = lcd_panel;
    lcd_width_ = lcd_width;
    lcd_height_ = lcd_height;
    audio_codec_ = codec;
    sd_card_ = sd_card;
    mount_point_ = sd_card_->GetMountPoint();

    if (audio_pcm_buf_ == nullptr) {
        audio_pcm_buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(kAudioPcmBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!audio_pcm_buf_) {
            ESP_LOGE(TAG, "Failed to alloc audio buf (%zu)", kAudioPcmBufferSize);
            Deinitialize();
            return false;
        }
        audio_pcm_buf_size_ = kAudioPcmBufferSize;
    }

    // Allocate pending H264 frame buffer for playback→render handoff
    if (pending_h264_buf_ == nullptr) {
        pending_h264_buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!pending_h264_buf_) {
            ESP_LOGE(TAG, "Failed to alloc pending H264 buf");
            Deinitialize();
            return false;
        }
    }
    if (decode_h264_buf_ == nullptr) {
        decode_h264_buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!decode_h264_buf_) {
            ESP_LOGE(TAG, "Failed to alloc decode H264 buf");
            Deinitialize();
            return false;
        }
    }

    // Create synchronization primitives for decode/render separation
    frame_ready_sem_ = xSemaphoreCreateBinary();
    h264_mutex_ = xSemaphoreCreateMutex();
    if (!frame_ready_sem_ || !h264_mutex_) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        Deinitialize();
        return false;
    }

    // Register all supported extractors (MP4, etc.)
    esp_extractor_register_default();

    // Register all supported audio decoders (MP3, etc.)
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    // Create independent render task
    render_exit_.store(false);
#if MP4_PLAYER_STATIC_TASK_CREATION == 1
    if (render_task_stack_ == nullptr) {
        render_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(MP4_RENDER_TASK_STACK, MALLOC_CAP_SPIRAM));
        if (!render_task_stack_) {
            ESP_LOGE(TAG, "Failed to alloc render task stack");
            Deinitialize();
            return false;
        }
    }
    if (render_task_buffer_ == nullptr) {
        render_task_buffer_ = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        if (!render_task_buffer_) {
            ESP_LOGE(TAG, "Failed to alloc render task buffer");
            Deinitialize();
            return false;
        }
    }
    render_task_handle_ = xTaskCreateStaticPinnedToCore(
        RenderTaskEntry, "mp4_render",
        MP4_RENDER_TASK_STACK, this,
        MP4_RENDER_TASK_PRIORITY,
        render_task_stack_, render_task_buffer_, MP4_RENDER_TASK_CORE);
#else
    BaseType_t ret = xTaskCreatePinnedToCore(
        RenderTaskEntry, "mp4_render",
        MP4_RENDER_TASK_STACK, this,
        MP4_RENDER_TASK_PRIORITY,
        &render_task_handle_, MP4_RENDER_TASK_CORE);
    if (ret != pdPASS) {
        render_task_handle_ = nullptr;
    }
#endif
    if (!render_task_handle_) {
        ESP_LOGE(TAG, "Failed to create render task");
        Deinitialize();
        return false;
    }
    ESP_LOGI(TAG, "Render task created: core=%d prio=%d", MP4_RENDER_TASK_CORE, MP4_RENDER_TASK_PRIORITY);

    initialized_.store(true);
    SetState(Mp4PlayerState::Idle);
    ESP_LOGI(TAG, "Initialized: LCD %ux%u, mount=%s", lcd_width_, lcd_height_, mount_point_.c_str());
    return true;
}

void Mp4Player::Deinitialize() {
    Stop();

    // Stop render task
    if (render_task_handle_) {
        render_exit_.store(true);
        if (frame_ready_sem_) {
            xSemaphoreGive(frame_ready_sem_);  // Wake render task so it can exit
        }
        // Wait for task to finish (max 2 seconds)
        int timeout = 100;
        while (render_task_running_.load() && --timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (timeout <= 0) {
            ESP_LOGW(TAG, "Render task did not exit in time, force deleting");
            vTaskDelete(render_task_handle_);
        }
        render_task_handle_ = nullptr;
#if MP4_PLAYER_STATIC_TASK_CREATION == 1
        if (render_task_buffer_) {
            heap_caps_free(render_task_buffer_);
            render_task_buffer_ = nullptr;
        }
        if (render_task_stack_) {
            heap_caps_free(render_task_stack_);
            render_task_stack_ = nullptr;
        }
#endif
        render_exit_.store(false);
    }

    // Now safe to close decoders (render task is stopped)
    CloseDecoders();

    if (audio_pcm_buf_) {
        heap_caps_free(audio_pcm_buf_);
        audio_pcm_buf_ = nullptr;
        audio_pcm_buf_size_ = 0;
    }
    if (video_rgb565_buf_) {
        heap_caps_free(video_rgb565_buf_);
        video_rgb565_buf_ = nullptr;
        video_rgb565_buf_size_ = 0;
    }
    if (pending_h264_buf_) {
        heap_caps_free(pending_h264_buf_);
        pending_h264_buf_ = nullptr;
    }
    if (decode_h264_buf_) {
        heap_caps_free(decode_h264_buf_);
        decode_h264_buf_ = nullptr;
    }
    if (frame_ready_sem_) {
        vSemaphoreDelete(frame_ready_sem_);
        frame_ready_sem_ = nullptr;
    }
    if (h264_mutex_) {
        vSemaphoreDelete(h264_mutex_);
        h264_mutex_ = nullptr;
    }

    initialized_.store(false);
    SetState(Mp4PlayerState::Idle);
    ESP_LOGI(TAG, "Deinitialized");
}

bool Mp4Player::Play(const std::string& file_path) {
    if (!initialized_.load()) {
        ESP_LOGE(TAG, "Not initialized");
        return false;
    }

    if (state_.load() != Mp4PlayerState::Idle) {
        Stop();
    }

    SetState(Mp4PlayerState::Loading);
    stop_requested_.store(false);
    paused_.store(false);
    current_file_path_ = file_path;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = Mp4PlaybackStats{};
    }

    if (!PreparePlayback(file_path)) {
        CleanupPlaybackResources();
        SetState(Mp4PlayerState::Error);
        return false;
    }

#if MP4_PLAYER_STATIC_TASK_CREATION == 1
    if (playback_task_stack_ == nullptr) {
        playback_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(MP4_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM));
    }
    if (playback_task_buffer_ == nullptr) {
        playback_task_buffer_ = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    playback_task_handle_ = xTaskCreateStaticPinnedToCore(
        PlaybackTaskEntry, "mp4_play",
        MP4_TASK_STACK_SIZE, this,
        MP4_TASK_PRIORITY,
        playback_task_stack_, playback_task_buffer_, MP4_TASK_CORE);
#else
    BaseType_t ret = xTaskCreatePinnedToCore(
        PlaybackTaskEntry, "mp4_play",
        MP4_TASK_STACK_SIZE, this,
        MP4_TASK_PRIORITY,
        &playback_task_handle_, MP4_TASK_CORE);
    if (ret != pdPASS) {
        playback_task_handle_ = nullptr;
    }
#endif
    if (!playback_task_handle_) {
        ESP_LOGE(TAG, "Failed to create playback task");
        CleanupPlaybackResources();
        SetState(Mp4PlayerState::Error);
        return false;
    }

    playback_task_running_.store(true);
    SetState(Mp4PlayerState::Playing);
    ESP_LOGI(TAG, "Playing: %s", file_path.c_str());
    return true;
}

bool Mp4Player::PlayFile(const std::string& file_name) {
    return Play(BuildFullPath(file_name));
}

void Mp4Player::Stop() {
    if (state_.load() == Mp4PlayerState::Idle) {
        return;
    }

    stop_requested_.store(true);
    SetState(Mp4PlayerState::Stopping);

    int guard = 100;
    while (playback_task_running_.load() && --guard >= 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    CleanupPlaybackResources();
    stop_requested_.store(false);
    paused_.store(false);
    playback_task_running_.store(false);
    current_file_path_.clear();
    SetState(Mp4PlayerState::Idle);
    ESP_LOGI(TAG, "Stopped");
}

void Mp4Player::Pause() {
    if (state_.load() != Mp4PlayerState::Playing) return;
    paused_.store(true);
    SetState(Mp4PlayerState::Paused);
}

void Mp4Player::Resume() {
    if (state_.load() != Mp4PlayerState::Paused) return;
    paused_.store(false);
    SetState(Mp4PlayerState::Playing);
}

bool Mp4Player::Next() {
    if (playlist_.empty()) return false;
    int next = current_index_ + 1;
    if (next >= static_cast<int>(playlist_.size())) next = 0;
    current_index_ = next;
    return Play(playlist_[current_index_].path);
}

bool Mp4Player::Prev() {
    if (playlist_.empty()) return false;
    int prev = current_index_ - 1;
    if (prev < 0) prev = static_cast<int>(playlist_.size()) - 1;
    current_index_ = prev;
    return Play(playlist_[current_index_].path);
}

void Mp4Player::SetDirectory(const std::string& dir) {
    mp4_directory_ = dir;
    playlist_.clear();
    current_index_ = -1;
}

size_t Mp4Player::ScanDirectory() {
    playlist_.clear();
    current_index_ = -1;

    std::string dir_path = mount_point_ + "/" + mp4_directory_;
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open directory: %s", dir_path.c_str());
        return 0;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr && playlist_.size() < MP4_MAX_FILES) {
        if (entry->d_type != DT_REG) continue;
        std::string name(entry->d_name);
        if (!IsSupportedMp4Name(name)) continue;

        Mp4FileInfo info;
        info.name = name;
        info.path = dir_path + "/" + name;

        struct stat st;
        if (stat(info.path.c_str(), &st) == 0) {
            info.file_size = static_cast<uint32_t>(st.st_size);
        }
        playlist_.push_back(std::move(info));
    }
    closedir(dir);

    std::sort(playlist_.begin(), playlist_.end(),
              [](const Mp4FileInfo& a, const Mp4FileInfo& b) { return a.name < b.name; });

    ESP_LOGI(TAG, "Scanned %zu MP4 files in %s", playlist_.size(), dir_path.c_str());
    return playlist_.size();
}

Mp4PlaybackStats Mp4Player::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// File I/O callbacks for esp_extractor
// ---------------------------------------------------------------------------

void Mp4Player::PlaybackTaskEntry(void* arg) {
    auto* self = static_cast<Mp4Player*>(arg);
    if (self) self->PlaybackTaskLoop();
}

void Mp4Player::RenderTaskEntry(void* arg) {
    auto* self = static_cast<Mp4Player*>(arg);
    if (self) self->RenderTaskLoop();
}

int Mp4Player::FileReadCb(void* buffer, uint32_t size, void* ctx) {
    auto* fc = static_cast<FileContext*>(ctx);
    if (!fc || !fc->fp) return -1;
    return static_cast<int>(fread(buffer, 1, size, fc->fp));
}

int Mp4Player::FileSeekCb(uint32_t position, void* ctx) {
    auto* fc = static_cast<FileContext*>(ctx);
    if (!fc || !fc->fp) return -1;
    return fseek(fc->fp, static_cast<long>(position), SEEK_SET);
}

uint32_t Mp4Player::FileSizeCb(void* ctx) {
    auto* fc = static_cast<FileContext*>(ctx);
    if (!fc || !fc->fp) return 0;
    long cur = ftell(fc->fp);
    if (cur < 0) return 0;
    fseek(fc->fp, 0, SEEK_END);
    long end = ftell(fc->fp);
    fseek(fc->fp, cur, SEEK_SET);
    return end < 0 ? 0 : static_cast<uint32_t>(end);
}

// ---------------------------------------------------------------------------
// Prepare / cleanup
// ---------------------------------------------------------------------------

bool Mp4Player::PreparePlayback(const std::string& file_path) {
    if (file_path.empty()) {
        ESP_LOGE(TAG, "Empty file path");
        return false;
    }

    CloseDecoders();
    CloseFile();

    file_ctx_.fp = fopen(file_path.c_str(), "rb");
    if (!file_ctx_.fp) {
        ESP_LOGE(TAG, "Failed to open: %s", file_path.c_str());
        return false;
    }

    // Fill extractor config manually (not using helper to avoid extra dependency)
    memset(&extractor_cfg_, 0, sizeof(extractor_cfg_));
    extractor_cfg_.type = ESP_EXTRACTOR_TYPE_MP4;
    extractor_cfg_.extract_mask = ESP_EXTRACT_MASK_AV;
    extractor_cfg_.in_read_cb = FileReadCb;
    extractor_cfg_.in_seek_cb = FileSeekCb;
    extractor_cfg_.in_size_cb = FileSizeCb;
    extractor_cfg_.in_ctx = &file_ctx_;
    extractor_cfg_.out_pool_size = MP4_EXTRACTOR_POOL_SIZE;
    extractor_cfg_.out_align = 16;

    esp_extractor_err_t err = esp_extractor_open(&extractor_cfg_, &extractor_);
    if (err != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "esp_extractor_open failed: %d", err);
        return false;
    }

    err = esp_extractor_parse_stream(extractor_);
    if (err != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "esp_extractor_parse_stream failed: %d", err);
        return false;
    }

    // Query streams
    uint16_t audio_num = 0, video_num = 0;
    err = esp_extractor_get_stream_num(extractor_, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, &audio_num);
    if (err != ESP_EXTRACTOR_ERR_OK || audio_num == 0) {
        ESP_LOGE(TAG, "No audio stream in %s", file_path.c_str());
        return false;
    }
    err = esp_extractor_get_stream_num(extractor_, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, &video_num);
    if (err != ESP_EXTRACTOR_ERR_OK || video_num == 0) {
        ESP_LOGE(TAG, "No video stream in %s", file_path.c_str());
        return false;
    }

    esp_extractor_stream_info_t audio_info = {};
    esp_extractor_stream_info_t video_info = {};
    err = esp_extractor_get_stream_info(extractor_, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0, &audio_info);
    if (err != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "Failed to get audio info: %d", err);
        return false;
    }
    esp_extractor_audio_stream_info_t *audio_stream_info = &audio_info.audio_info;
    ESP_LOGI(TAG, "Audio format:%s sample_rate:%d channel:%d bits:%d duration:%d",
                esp_extractor_get_format_name(audio_stream_info->format),
                (int)audio_stream_info->sample_rate, (int)audio_stream_info->channel, (int)audio_stream_info->bits_per_sample,
                (int)audio_info.duration);

    err = esp_extractor_get_stream_info(extractor_, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0, &video_info);
    if (err != ESP_EXTRACTOR_ERR_OK) {
        ESP_LOGE(TAG, "Failed to get video info: %d", err);
        return false;
    }
    esp_extractor_video_stream_info_t *video_stream_info = &video_info.video_info;
    ESP_LOGI(TAG, "Video format:%s resolution:%dx%d fps:%d dur:%d",
                esp_extractor_get_format_name(video_stream_info->format),
                video_stream_info->width, video_stream_info->height, video_stream_info->fps,
                (int)video_info.duration);

    // Validate audio format: MP3, AAC, or FLAC
    if (audio_info.audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_MP3 &&
        audio_info.audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_AAC &&
        audio_info.audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_FLAC) {
        ESP_LOGE(TAG, "Unsupported audio fmt: 0x%lx (only MP3/AAC/FLAC)", (unsigned long)audio_info.audio_info.format);
        return false;
    }

    if (video_info.video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_H264) {
        ESP_LOGE(TAG, "Unsupported video fmt: 0x%lx", (unsigned long)video_info.video_info.format);
        return false;
    }

    if (!InitAudioDecoder(audio_stream_info->format,
                          audio_stream_info->sample_rate,
                          audio_stream_info->channel,
                          audio_stream_info->bits_per_sample)) {
        return false;
    }
    if (!InitVideoDecoder(video_stream_info->width,
                          video_stream_info->height,
                          video_stream_info->fps)) {
        return false;
    }

    return true;
}

void Mp4Player::CleanupPlaybackResources() {
    // Don't close decoders here - render task may still be using them
    // Decoders are closed in Deinitialize() after render task stops
    if (extractor_) {
        esp_extractor_close(extractor_);
        extractor_ = nullptr;
    }
    if (audio_codec_) {
        audio_codec_->SetOutputSampleRate(-1);
    }
    CloseFile();
}

void Mp4Player::CloseFile() {
    if (file_ctx_.fp) {
        fclose(file_ctx_.fp);
        file_ctx_.fp = nullptr;
    }
}

void Mp4Player::CloseDecoders() {
    if (audio_dec_) {
        esp_audio_simple_dec_close(audio_dec_);
        audio_dec_ = nullptr;
    }
    if (video_dec_) {
        esp_h264_dec_close(video_dec_);
        esp_h264_dec_del(video_dec_);
        video_dec_ = nullptr;
    }
    video_param_ = nullptr;
    if (color_convert_) {
        esp_imgfx_color_convert_close(color_convert_);
        color_convert_ = nullptr;
    }
    if (video_rgb565_buf_) {
        heap_caps_free(video_rgb565_buf_);
        video_rgb565_buf_ = nullptr;
        video_rgb565_buf_size_ = 0;
    }
    video_width_ = 0;
    video_height_ = 0;
}

// ---------------------------------------------------------------------------
// Decoder init
// ---------------------------------------------------------------------------

bool Mp4Player::InitAudioDecoder(uint32_t audio_format, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    if (audio_dec_) {
        esp_audio_simple_dec_close(audio_dec_);
        audio_dec_ = nullptr;
    }

    esp_audio_simple_dec_cfg_t cfg = {};
    // Map extractor audio format to simple decoder type
    switch (audio_format) {
        case ESP_EXTRACTOR_AUDIO_FORMAT_AAC:
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            ESP_LOGI(TAG, "Audio decoder: AAC");
            break;
        case ESP_EXTRACTOR_AUDIO_FORMAT_FLAC:
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
            ESP_LOGI(TAG, "Audio decoder: FLAC");
            break;
        case ESP_EXTRACTOR_AUDIO_FORMAT_MP3:
        default:
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
            ESP_LOGI(TAG, "Audio decoder: MP3");
            break;
    }
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    cfg.use_frame_dec = true;  // extractor provides complete frames

    esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &audio_dec_);
    if (err != ESP_AUDIO_ERR_OK || !audio_dec_) {
        ESP_LOGE(TAG, "esp_audio_simple_dec_open failed: %d", err);
        audio_dec_ = nullptr;
        return false;
    }

    if (audio_codec_) {
        audio_codec_->EnableOutput(true);
        audio_codec_->SetOutputSampleRate(static_cast<int>(sample_rate));
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.audio_rate = sample_rate;
        stats_.audio_channels = channels;
        stats_.audio_bits = bits_per_sample;
    }
    return true;
}

bool Mp4Player::InitVideoDecoder(uint16_t width, uint16_t height, uint16_t fps) {
    // Close previous
    if (video_dec_) {
        esp_h264_dec_close(video_dec_);
        esp_h264_dec_del(video_dec_);
        video_dec_ = nullptr;
    }
    if (color_convert_) {
        esp_imgfx_color_convert_close(color_convert_);
        color_convert_ = nullptr;
    }
    if (video_rgb565_buf_) {
        heap_caps_free(video_rgb565_buf_);
        video_rgb565_buf_ = nullptr;
        video_rgb565_buf_size_ = 0;
    }

    // Create H264 SW decoder
    esp_h264_dec_cfg_sw_t cfg = {};
    cfg.pic_type = ESP_H264_RAW_FMT_I420;
    esp_h264_err_t h_err = esp_h264_dec_sw_new(&cfg, &video_dec_);
    if (h_err != ESP_H264_ERR_OK || !video_dec_) {
        ESP_LOGE(TAG, "esp_h264_dec_sw_new failed: %d", h_err);
        video_dec_ = nullptr;
        return false;
    }

    h_err = esp_h264_dec_open(video_dec_);
    if (h_err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_dec_open failed: %d", h_err);
        return false;
    }

    h_err = esp_h264_dec_sw_get_param_hd(video_dec_, &video_param_);
    if (h_err != ESP_H264_ERR_OK || !video_param_) {
        ESP_LOGE(TAG, "get_param_hd failed: %d", h_err);
        video_param_ = nullptr;
        return false;
    }

    // Use resolution from container metadata
    video_width_ = width;
    video_height_ = height;

    if (video_width_ == 0 || video_height_ == 0) {
        ESP_LOGE(TAG, "Invalid video resolution %ux%u", video_width_, video_height_);
        return false;
    }
    if (video_width_ > lcd_width_ || video_height_ > lcd_height_) {
        ESP_LOGW(TAG, "Video %ux%u exceeds LCD %ux%u, will crop to fit",
                 video_width_, video_height_, lcd_width_, lcd_height_);
    }

    // Allocate RGB565 output buffer
    esp_imgfx_resolution_t res = {
        .width = static_cast<int16_t>(video_width_),
        .height = static_cast<int16_t>(video_height_),
    };
    uint32_t out_size = 0;
    esp_imgfx_err_t img_err = esp_imgfx_get_image_size(ESP_IMGFX_PIXEL_FMT_RGB565_BE, &res, &out_size);
    if (img_err != ESP_IMGFX_ERR_OK || out_size == 0) {
        ESP_LOGE(TAG, "esp_imgfx_get_image_size failed: %d", img_err);
        return false;
    }

    video_rgb565_buf_ = static_cast<uint8_t*>(
        heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!video_rgb565_buf_) {
        ESP_LOGE(TAG, "Failed to alloc video buf (%lu)", (unsigned long)out_size);
        return false;
    }
    video_rgb565_buf_size_ = out_size;

    // Color converter: I420 -> RGB565_BE
    esp_imgfx_color_convert_cfg_t cc_cfg = {
        .in_res = res,
        .in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_I420,
        .out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB565_BE,
        .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
    };
    img_err = esp_imgfx_color_convert_open(&cc_cfg, &color_convert_);
    if (img_err != ESP_IMGFX_ERR_OK || !color_convert_) {
        ESP_LOGE(TAG, "color_convert_open failed: %d", img_err);
        color_convert_ = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.video_width = video_width_;
        stats_.video_height = video_height_;
        stats_.video_fps = fps;
    }
    ESP_LOGI(TAG, "Video decoder ready: %ux%u fps=%u", video_width_, video_height_, fps);
    return true;
}

// ---------------------------------------------------------------------------
// Frame processing
// ---------------------------------------------------------------------------

bool Mp4Player::ProcessAudioFrame(const esp_extractor_frame_info_t& frame) {
    if (!audio_dec_ || frame.frame_size == 0 || frame.frame_buffer == nullptr) {
        return false;
    }

    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = frame.frame_buffer;
    raw.len = frame.frame_size;
    raw.eos = EXTRACTOR_IS_EOS(frame.frame_flag);
    raw.consumed = 0;
    raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;

    esp_audio_simple_dec_out_t out = {};
    out.buffer = audio_pcm_buf_;
    out.len = static_cast<uint32_t>(audio_pcm_buf_size_);
    out.needed_size = 0;
    out.decoded_size = 0;

    // Process all data in the frame (may contain multiple encoded frames)
    while (raw.len > 0) {
        out.decoded_size = 0;
        out.needed_size = 0;
        out.buffer = audio_pcm_buf_;
        out.len = static_cast<uint32_t>(audio_pcm_buf_size_);

        esp_audio_err_t err = esp_audio_simple_dec_process(audio_dec_, &raw, &out);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > audio_pcm_buf_size_) {
            // Grow buffer
            heap_caps_free(audio_pcm_buf_);
            audio_pcm_buf_ = static_cast<uint8_t*>(
                heap_caps_malloc(out.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            audio_pcm_buf_size_ = audio_pcm_buf_ ? out.needed_size : 0;
            if (!audio_pcm_buf_) {
                ESP_LOGE(TAG, "Failed to grow audio buf");
                return false;
            }
            continue;  // retry with bigger buffer
        }
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "audio dec process err: %d", err);
            return false;
        }

        // Advance input past consumed bytes
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;

        if (out.decoded_size > 0 && out.buffer) {
            // Get decode info for sample rate updates
            esp_audio_simple_dec_info_t info = {};
            if (esp_audio_simple_dec_get_info(audio_dec_, &info) == ESP_AUDIO_ERR_OK) {
                size_t total_samples = out.decoded_size / sizeof(int16_t);
                auto* pcm = reinterpret_cast<int16_t*>(out.buffer);
                if (audio_callback_) {
                    audio_callback_(pcm, total_samples, info.channel);
                }
                OutputPcmToCodec(pcm, total_samples, info.channel);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.audio_frames++;
    }
    return true;
}

bool Mp4Player::ProcessVideoFrame(const esp_extractor_frame_info_t& frame) {
    // Playback task: just queue H264 frame for render task
    if (frame.frame_size == 0 || frame.frame_buffer == nullptr) {
        return false;
    }

    // Copy H264 frame to pending buffer (mutex protected)
    xSemaphoreTake(h264_mutex_, portMAX_DELAY);
    if (frame.frame_size <= 512 * 1024) {
        memcpy(pending_h264_buf_, frame.frame_buffer, frame.frame_size);
        pending_h264_size_ = frame.frame_size;
        pending_frame_w_ = video_width_;
        pending_frame_h_ = video_height_;
    } else {
        ESP_LOGW(TAG, "H264 frame too large: %lu", (unsigned long)frame.frame_size);
        pending_h264_size_ = 0;
    }
    xSemaphoreGive(h264_mutex_);

    // Signal render task that frame is ready
    xSemaphoreGive(frame_ready_sem_);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.video_frames++;
    }
    return true;
}

bool Mp4Player::DrawFrameToLcd(uint16_t width, uint16_t height) {
    if (!lcd_panel_ || !video_rgb565_buf_) return false;

    uint16_t draw_w = std::min(width, lcd_width_);
    uint16_t draw_h = std::min(height, lcd_height_);

    // Center the frame on LCD if smaller
    int x_offset = (lcd_width_ - draw_w) / 2;
    int y_offset = (lcd_height_ - draw_h) / 2;

    // If video fits horizontally, use direct row-chunked drawing
    if (width <= lcd_width_) {
        for (uint16_t row = 0; row < draw_h; row += MP4_BUFFER_LINES) {
            uint16_t row_end = std::min(static_cast<uint16_t>(row + MP4_BUFFER_LINES), draw_h);
            uint16_t* src_row = reinterpret_cast<uint16_t*>(video_rgb565_buf_) + (row * width);
            
            esp_err_t err = esp_lcd_panel_draw_bitmap(lcd_panel_,
                x_offset, y_offset + row,
                x_offset + draw_w, y_offset + row_end,
                src_row);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "draw_bitmap chunk failed: %s", esp_err_to_name(err));
                return false;
            }
        }
        return true;
    }

    // Video wider than LCD: crop each row
    size_t row_size = draw_w * sizeof(uint16_t);
    auto* cropped_row = static_cast<uint8_t*>(heap_caps_malloc(row_size * MP4_BUFFER_LINES, MALLOC_CAP_SPIRAM));
    if (!cropped_row) {
        ESP_LOGW(TAG, "Failed to alloc crop buffer");
        return false;
    }

    bool success = true;
    size_t src_stride = width * sizeof(uint16_t);
    
    for (uint16_t row = 0; row < draw_h; row += MP4_BUFFER_LINES) {
        uint16_t row_end = std::min(static_cast<uint16_t>(row + MP4_BUFFER_LINES), draw_h);
        uint16_t rows_to_copy = row_end - row;
        
        // Copy cropped rows into buffer
        for (uint16_t r = 0; r < rows_to_copy; ++r) {
            uint8_t* src = video_rgb565_buf_ + (row + r) * src_stride;
            uint8_t* dst = cropped_row + r * row_size;
            memcpy(dst, src, row_size);
        }
        
        // Draw the cropped chunk
        esp_err_t err = esp_lcd_panel_draw_bitmap(lcd_panel_,
            x_offset, y_offset + row,
            x_offset + draw_w, y_offset + row_end,
            cropped_row);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap chunk failed: %s", esp_err_to_name(err));
            success = false;
            break;
        }
    }
    
    heap_caps_free(cropped_row);
    return success;
}

void Mp4Player::OutputPcmToCodec(const int16_t* pcm, size_t samples, int channels) {
    if (!audio_codec_ || !pcm || samples == 0) return;

    int dst_ch = audio_codec_->output_channels();
    if (dst_ch <= 0) dst_ch = channels;

    std::vector<int16_t> out;
    if (dst_ch == channels) {
        out.assign(pcm, pcm + samples);
    } else if (channels == 1 && dst_ch == 2) {
        out.reserve(samples * 2);
        for (size_t i = 0; i < samples; ++i) {
            int16_t v = ClampToInt16(static_cast<float>(pcm[i]) * volume_factor_);
            out.push_back(v);
            out.push_back(v);
        }
    } else if (channels == 2 && dst_ch == 1) {
        out.reserve(samples / 2);
        for (size_t i = 0; i + 1 < samples; i += 2) {
            float mixed = (static_cast<float>(pcm[i]) + pcm[i + 1]) * 0.5f * volume_factor_;
            out.push_back(ClampToInt16(mixed));
        }
    } else {
        out.assign(pcm, pcm + samples);
    }

    // Apply volume if not already done in channel conversion
    if (dst_ch == channels && volume_factor_ != 1.0f) {
        for (auto& s : out) {
            s = ClampToInt16(static_cast<float>(s) * volume_factor_);
        }
    }

    audio_codec_->OutputData(out);
}

void Mp4Player::SetState(Mp4PlayerState new_state) {
    Mp4PlayerState old_state = state_.exchange(new_state);
    if (old_state == new_state) return;
    if (state_callback_) {
        state_callback_(old_state, new_state);
    }
}

std::string Mp4Player::BuildFullPath(const std::string& filename) const {
    if (filename.empty()) return {};
    if (filename.front() == '/' || filename.find(':') != std::string::npos) return filename;
    return mount_point_ + "/" + mp4_directory_ + "/" + filename;
}

bool Mp4Player::IsSupportedMp4Name(const std::string& name) const {
    if (name.size() < 5) return false;
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return ext == ".mp4";
}

// ---------------------------------------------------------------------------
// Playback task
// ---------------------------------------------------------------------------

void Mp4Player::PlaybackTaskLoop() {
    while (!stop_requested_.load()) {
        if (paused_.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        esp_extractor_frame_info_t frame = {};
        esp_extractor_err_t err = esp_extractor_read_frame(extractor_, &frame);
        if (err == ESP_EXTRACTOR_ERR_EOS) {
            break;
        }
        if (err == ESP_EXTRACTOR_ERR_SKIPPED) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.dropped_frames++;
            continue;
        }
        if (err == ESP_EXTRACTOR_ERR_WAITING_OUTPUT) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (err != ESP_EXTRACTOR_ERR_OK) {
            ESP_LOGW(TAG, "read_frame err: %d", err);
            break;
        }

        bool ok = true;
        if (frame.frame_buffer) {
            if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO) {
                ok = ProcessAudioFrame(frame);
            } else if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_VIDEO) {
                ok = ProcessVideoFrame(frame);
            }
        }
        esp_extractor_release_frame(extractor_, &frame);

        if (!ok) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.dropped_frames++;
        }

        if (frame.frame_flag & EXTRACTOR_FRAME_FLAG_EOS) {
            break;
        }
    }

    bool should_repeat = false;
    std::string ended_path = current_file_path_;
    if (!stop_requested_.load()) {
        should_repeat = end_callback_ ? end_callback_(ended_path) : false;
    }

    playback_task_running_.store(false);
    CleanupPlaybackResources();
    current_file_path_.clear();
    stop_requested_.store(false);
    paused_.store(false);

    if (!should_repeat) {
        SetState(Mp4PlayerState::Idle);
    }
    ESP_LOGI(TAG, "Playback finished: %s", ended_path.c_str());
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Render task - decode H264 and draw to LCD (independent core 0)
// ---------------------------------------------------------------------------

void Mp4Player::RenderTaskLoop() {
    ESP_LOGI(TAG, "Render task started on core %d", xPortGetCoreID());
    render_task_running_.store(true);

    while (true) {
        // Wait for a new H264 frame signal (or timeout to check exit flag)
        if (xSemaphoreTake(frame_ready_sem_, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (render_exit_.load()) break;
            continue;
        }

        if (render_exit_.load()) break;
        if (stop_requested_.load() || paused_.load()) continue;

        // Copy H264 frame from pending buffer to decode buffer
        uint32_t h264_size;
        uint16_t frame_w, frame_h;
        xSemaphoreTake(h264_mutex_, portMAX_DELAY);
        h264_size = pending_h264_size_;
        frame_w = pending_frame_w_;
        frame_h = pending_frame_h_;
        if (h264_size > 0) {
            memcpy(decode_h264_buf_, pending_h264_buf_, h264_size);
        }
        xSemaphoreGive(h264_mutex_);

        if (h264_size == 0 || !video_dec_ || !color_convert_) {
            continue;
        }

        // H264 decode in render task
        esp_h264_dec_in_frame_t in_frame = {};
        in_frame.raw_data.buffer = decode_h264_buf_;
        in_frame.raw_data.len = h264_size;
        in_frame.pts = 0;
        in_frame.dts = 0;

        while (in_frame.raw_data.len > 0) {
            in_frame.consume = 0;
            esp_h264_dec_out_frame_t out_frame = {};

            esp_h264_err_t err = esp_h264_dec_process(video_dec_, &in_frame, &out_frame);
            if (err != ESP_H264_ERR_OK) {
                ESP_LOGW(TAG, "h264 dec err: %d", err);
                break;
            }

            // Advance past consumed bytes
            if (in_frame.consume > 0) {
                in_frame.raw_data.buffer += in_frame.consume;
                in_frame.raw_data.len -= in_frame.consume;
            } else {
                break;
            }

            if (out_frame.outbuf == nullptr || out_frame.out_size == 0) {
                continue;  // SPS/PPS or buffering
            }

            // Resolution from container or decoder
            if (video_width_ == 0 || video_height_ == 0) {
                video_width_ = frame_w;
                video_height_ = frame_h;
            }

            // Color convert I420 -> RGB565
            esp_imgfx_data_t in_image = {
                .data = out_frame.outbuf,
                .data_len = out_frame.out_size,
            };
            esp_imgfx_data_t out_image = {
                .data = video_rgb565_buf_,
                .data_len = video_rgb565_buf_size_,
            };
            esp_imgfx_err_t conv_err = esp_imgfx_color_convert_process(color_convert_, &in_image, &out_image);
            if (conv_err != ESP_IMGFX_ERR_OK) {
                ESP_LOGW(TAG, "color convert err: %d", conv_err);
                break;
            }

            // Frame callback
            if (frame_callback_) {
                frame_callback_(reinterpret_cast<uint16_t*>(video_rgb565_buf_), video_width_, video_height_);
            }

            // Draw to LCD
            DrawFrameToLcd(video_width_, video_height_);
        }
    }

    render_task_running_.store(false);
    ESP_LOGI(TAG, "Render task exiting");
    vTaskDelete(NULL);
}
