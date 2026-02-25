#ifndef AUDIO_STREAM_PLAYER_H
#define AUDIO_STREAM_PLAYER_H

/**
 * @file audio_stream_player.h
 * @brief Base class for HTTP audio stream players (Music & Radio)
 *
 * Provides common infrastructure:
 *   - HTTP streaming download (producer task)
 *   - Ring-buffer with back-pressure
 *   - Decoder lifecycle (esp_audio_codec: MP3/AAC)
 *   - PCM output, mono down-mix, volume amplification
 *   - FFT display feeding
 *   - FreeRTOS tasks pinned to specific cores
 *
 * Subclasses override hooks to customise behaviour per use-case.
 */

#include <string>
#include <queue>
#include <atomic>
#include <vector>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

extern "C" {
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
}

/* ------------------------------------------------------------------ */
/*  Macros – eliminates magic numbers across the whole module          */
/* ------------------------------------------------------------------ */

/** Maximum audio buffer size (bytes) – limits PSRAM usage */
#define AUDIO_BUF_MAX_SIZE          (256 * 1024)

/** Minimum buffered data before playback starts (bytes) */
#define AUDIO_BUF_MIN_SIZE          (32 * 1024)

/** Per-chunk read size from HTTP stream (bytes) */
#define AUDIO_HTTP_CHUNK_SIZE       4096

/** Decoder input buffer size (bytes) */
#define AUDIO_DEC_INPUT_BUF_SIZE    8192

/** Default PCM output buffer (enough for 1 MP3 frame: 1152*2ch*2B) */
#define AUDIO_PCM_OUT_BUF_SIZE      4608

/** Stack size for the download task (bytes) */
#define AUDIO_DOWNLOAD_TASK_STACK   (4 * 1024)

/** Stack size for the playback task (bytes) */
#define AUDIO_PLAY_TASK_STACK       (6 * 1024)

/** Download task priority */
#define AUDIO_DOWNLOAD_TASK_PRIO    5

/** Playback task priority */
#define AUDIO_PLAY_TASK_PRIO        6

/** Core for download task (PRO_CPU = 0, APP_CPU = 1) */
#define AUDIO_DOWNLOAD_TASK_CORE    0

/** Core for playback task */
#define AUDIO_PLAY_TASK_CORE        1

/** Progress log interval (bytes) */
#define AUDIO_LOG_INTERVAL          (128 * 1024)

/** Default volume amplification factor */
#define AUDIO_DEFAULT_VOLUME        1.0f

/** Maximum reconnect attempts for streaming */
#define AUDIO_MAX_RECONNECT         3

/** Reconnect delay (ms) */
#define AUDIO_RECONNECT_DELAY_MS    1500

/** Buffer wait timeout when checking for idle state (ms) */
#define AUDIO_STATE_POLL_MS         50

/** Delay after toggling chat state (ms) */
#define AUDIO_CHAT_TOGGLE_DELAY_MS  300

/* ------------------------------------------------------------------ */
/*  Decoder type enum                                                 */
/* ------------------------------------------------------------------ */

enum class AudioDecoderType {
    MP3 = 0,
    AAC,
    AUTO   ///< Auto-detect from stream header
};

/* ------------------------------------------------------------------ */
/*  Audio data chunk (heap-allocated, lives in PSRAM)                 */
/* ------------------------------------------------------------------ */

struct StreamAudioChunk {
    uint8_t* data;
    size_t   size;

    StreamAudioChunk() : data(nullptr), size(0) {}
    StreamAudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

/* ------------------------------------------------------------------ */
/*  AudioStreamPlayer – abstract base class                           */
/* ------------------------------------------------------------------ */

class AudioStreamPlayer {
public:
    /** Display mode (shared by music & radio) */
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,
        DISPLAY_MODE_INFO     = 1
    };

    AudioStreamPlayer();
    virtual ~AudioStreamPlayer();

    /* ---- Common public API ---- */
    bool  StartStream(const std::string& url, AudioDecoderType type = AudioDecoderType::MP3);
    bool  StopStream();
    bool  IsPlaying()     const { return is_playing_.load(); }
    bool  IsDownloading() const { return is_downloading_.load(); }
    size_t GetBufferSize() const { return buffer_size_; }
    int16_t* GetAudioData()     { return fft_pcm_ptr_; }

    void SetDisplayMode(DisplayMode mode);
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }

    /** Set volume amplification (1.0 = 100%) */
    void SetVolume(float factor) { volume_factor_ = factor; }
    float GetVolume() const       { return volume_factor_; }

protected:
    /* ---- Hooks for subclasses ---- */

    /** Called before download starts. Override to add custom HTTP headers. */
    virtual void OnPrepareHttp(void* http_ptr) {}

    /** Called once after the first frame is decoded (sample_rate/bits/channels known).
     *  Override to display stream info on LCD. */
    virtual void OnStreamInfoReady(int sample_rate, int bits_per_sample, int channels) {}

    /** Called on each decoded PCM frame. Override for lyric sync, progress, etc. */
    virtual void OnPcmFrame(int64_t play_time_ms, int sample_rate, int channels) {}

    /** Called when playback finishes (naturally or stopped). */
    virtual void OnPlaybackFinished() {}

    /** Called when display should show station/song info. Override to customise. */
    virtual void OnDisplayReady() {}

    /* ---- Utility for subclasses ---- */
    int64_t GetPlayTimeMs() const { return current_play_time_ms_; }

    /** Access to the internal display mode for subclass logic */
    std::atomic<DisplayMode>& DisplayModeRef() { return display_mode_; }

private:
    /* ---- FreeRTOS task wrappers ---- */
    static void DownloadTaskEntry(void* param);
    static void PlayTaskEntry(void* param);

    void DownloadLoop(const std::string& url);
    void PlayLoop();

    /* ---- Buffer helpers ---- */
    void ClearAudioBuffer();
    bool WaitForBufferSpace();
    bool WaitForBufferData();

    /* ---- Decoder helpers ---- */
    bool  InitDecoder(AudioDecoderType type);
    void  CleanupDecoder();
    AudioDecoderType DetectStreamType(const uint8_t* data, size_t len);

    /* ---- Sample-rate reset ---- */
    void ResetSampleRate();

    /* ---- State ---- */
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::atomic<DisplayMode> display_mode_;
    float volume_factor_;

    /* ---- FreeRTOS handles ---- */
    TaskHandle_t download_task_handle_;
    TaskHandle_t play_task_handle_;

    /* ---- Audio buffer (producer-consumer) ---- */
    std::queue<StreamAudioChunk> audio_buffer_;
    SemaphoreHandle_t            buffer_mutex_;
    SemaphoreHandle_t            buffer_data_sem_;   ///< signalled when data is pushed
    SemaphoreHandle_t            buffer_space_sem_;  ///< signalled when data is popped
    size_t                       buffer_size_;

    /* ---- Decoder (esp_audio_codec) ---- */
    esp_audio_simple_dec_handle_t decoder_;
    esp_audio_simple_dec_info_t   dec_info_;
    bool                          decoder_initialized_;
    bool                          dec_info_ready_;
    AudioDecoderType              decoder_type_;
    uint8_t*                      pcm_out_buffer_;
    size_t                        pcm_out_buffer_size_;
    std::vector<uint8_t>          dec_out_vec_;   ///< resizeable output (AAC path)

    /* ---- Decoder input ---- */
    uint8_t* input_buffer_;
    int      input_bytes_left_;

    /* ---- Playback timing ---- */
    int64_t current_play_time_ms_;
    int     total_frames_decoded_;
    bool    fft_started_;
    bool    info_displayed_;

    /* ---- FFT ---- */
    int16_t* fft_pcm_ptr_;

    /* ---- URL for reconnect ---- */
    std::string stream_url_;
};

#endif // AUDIO_STREAM_PLAYER_H
