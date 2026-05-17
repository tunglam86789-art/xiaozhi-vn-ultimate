#ifndef MP4_PLAYER_H
#define MP4_PLAYER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_lcd_panel_ops.h>

extern "C" {
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_extractor.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_imgfx_color_convert.h"
#include "esp_imgfx_types.h"
}

class AudioCodec;
class SdCard;

#define MP4_PLAYER_STATIC_TASK_CREATION 1
#define MP4_DEFAULT_DIRECTORY            "videos"
#define MP4_MAX_FILES                    256
#define MP4_EXTRACTOR_POOL_SIZE          (256 * 1024)
#define MP4_AUDIO_BUF_SIZE               (16 * 1024)
#define MP4_TASK_STACK_SIZE              (12 * 1024)
#define MP4_TASK_PRIORITY                5
#define MP4_TASK_CORE                    1
#define MP4_RENDER_TASK_STACK            (10 * 1024)
#define MP4_RENDER_TASK_PRIORITY         4
#define MP4_RENDER_TASK_CORE             0
#define MP4_BUFFER_LINES                 40

enum class Mp4PlayerState {
    Idle = 0,
    Loading,
    Playing,
    Paused,
    Stopping,
    Error,
};

struct Mp4FileInfo {
    std::string name;
    std::string path;
    uint32_t file_size = 0;
};

struct Mp4PlaybackStats {
    uint32_t video_frames = 0;
    uint32_t audio_frames = 0;
    uint32_t dropped_frames = 0;
    uint16_t video_width = 0;
    uint16_t video_height = 0;
    uint16_t video_fps = 0;
    uint32_t audio_rate = 0;
    uint8_t audio_channels = 0;
    uint8_t audio_bits = 0;
};

using Mp4StateCallback = std::function<void(Mp4PlayerState old_state, Mp4PlayerState new_state)>;
using Mp4EndCallback = std::function<bool(const std::string& file_path)>;
using Mp4FrameCallback = std::function<void(uint16_t* rgb565, uint16_t width, uint16_t height)>;
using Mp4AudioCallback = std::function<void(int16_t* pcm, size_t samples, int channels)>;

class Mp4Player {
public:
    static Mp4Player& GetInstance();

    bool Initialize(esp_lcd_panel_handle_t lcd_panel,
                    uint16_t lcd_width, uint16_t lcd_height,
                    AudioCodec* codec,
                    SdCard* sd_card);
    void Deinitialize();

    bool Play(const std::string& file_path);
    bool PlayFile(const std::string& file_name);
    void Stop();
    void Pause();
    void Resume();
    bool Next();
    bool Prev();

    void SetDirectory(const std::string& dir);
    size_t ScanDirectory();

    const std::vector<Mp4FileInfo>& GetPlaylist() const { return playlist_; }
    int GetCurrentIndex() const { return current_index_; }

    Mp4PlayerState GetState() const { return state_.load(); }
    bool IsPlaying() const { return state_.load() == Mp4PlayerState::Playing; }
    bool IsPaused() const { return state_.load() == Mp4PlayerState::Paused; }
    bool IsInitialized() const { return initialized_.load(); }

    Mp4PlaybackStats GetStats() const;

    void SetStateCallback(Mp4StateCallback cb) { state_callback_ = std::move(cb); }
    void SetEndCallback(Mp4EndCallback cb) { end_callback_ = std::move(cb); }
    void SetFrameCallback(Mp4FrameCallback cb) { frame_callback_ = std::move(cb); }
    void SetAudioCallback(Mp4AudioCallback cb) { audio_callback_ = std::move(cb); }

    void SetVolume(float factor) { volume_factor_ = factor; }
    float GetVolume() const { return volume_factor_; }

private:
    struct FileContext {
        FILE* fp = nullptr;
    };

    Mp4Player();
    ~Mp4Player();
    Mp4Player(const Mp4Player&) = delete;
    Mp4Player& operator=(const Mp4Player&) = delete;

    static void PlaybackTaskEntry(void* arg);
    void PlaybackTaskLoop();

    static void RenderTaskEntry(void* arg);
    void RenderTaskLoop();

    static int FileReadCb(void* buffer, uint32_t size, void* ctx);
    static int FileSeekCb(uint32_t position, void* ctx);
    static uint32_t FileSizeCb(void* ctx);

    bool PreparePlayback(const std::string& file_path);
    void CleanupPlaybackResources();
    void CloseFile();
    void CloseDecoders();
    bool InitAudioDecoder(uint32_t audio_format, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    bool InitVideoDecoder(uint16_t width, uint16_t height, uint16_t fps);
    bool ProcessAudioFrame(const esp_extractor_frame_info_t& frame);
    bool ProcessVideoFrame(const esp_extractor_frame_info_t& frame);
    bool DrawFrameToLcd(uint16_t width, uint16_t height);
    void OutputPcmToCodec(const int16_t* pcm, size_t samples, int channels);
    void SetState(Mp4PlayerState new_state);
    std::string BuildFullPath(const std::string& filename) const;
    bool IsSupportedMp4Name(const std::string& name) const;

    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    uint16_t lcd_width_ = 0;
    uint16_t lcd_height_ = 0;
    AudioCodec* audio_codec_ = nullptr;
    SdCard* sd_card_ = nullptr;

    std::atomic<Mp4PlayerState> state_{Mp4PlayerState::Idle};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> playback_task_running_{false};
    std::atomic<bool> render_task_running_{false};
    std::atomic<bool> render_exit_{false};

    TaskHandle_t playback_task_handle_ = nullptr;
    TaskHandle_t render_task_handle_ = nullptr;
#if MP4_PLAYER_STATIC_TASK_CREATION == 1
    StackType_t* playback_task_stack_ = nullptr;
    StaticTask_t* playback_task_buffer_ = nullptr;
    StackType_t* render_task_stack_ = nullptr;
    StaticTask_t* render_task_buffer_ = nullptr;
#endif

    esp_extractor_handle_t extractor_ = nullptr;
    esp_extractor_config_t extractor_cfg_{};
    FileContext file_ctx_{};
    std::string current_file_path_;

    /* ---- Synchronization for decode/render separation ---- */
    SemaphoreHandle_t frame_ready_sem_ = nullptr;     ///< Signals new H264 frame ready
    SemaphoreHandle_t h264_mutex_ = nullptr;           ///< Protects pending H264 buffer

    /* ---- Pending H264 frame (from playback task to render task) ---- */
    uint8_t* pending_h264_buf_ = nullptr;              ///< Playback task writes H264 data
    uint32_t pending_h264_size_ = 0;
    uint16_t pending_frame_w_ = 0;
    uint16_t pending_frame_h_ = 0;
    uint8_t* decode_h264_buf_ = nullptr;               ///< Render task's private decode buffer

    /* ---- Decoders (owned by render task) ---- */
    esp_audio_simple_dec_handle_t audio_dec_ = nullptr;
    esp_h264_dec_handle_t video_dec_ = nullptr;
    esp_h264_dec_param_sw_handle_t video_param_ = nullptr;
    esp_imgfx_color_convert_handle_t color_convert_ = nullptr;

    uint8_t* audio_pcm_buf_ = nullptr;
    size_t audio_pcm_buf_size_ = 0;
    uint8_t* video_rgb565_buf_ = nullptr;
    uint32_t video_rgb565_buf_size_ = 0;
    uint16_t video_width_ = 0;
    uint16_t video_height_ = 0;

    mutable std::mutex stats_mutex_;
    Mp4PlaybackStats stats_{};

    std::string mp4_directory_ = MP4_DEFAULT_DIRECTORY;
    std::string mount_point_;
    std::vector<Mp4FileInfo> playlist_;
    int current_index_ = -1;

    float volume_factor_ = 1.0f;

    Mp4StateCallback state_callback_;
    Mp4EndCallback end_callback_;
    Mp4FrameCallback frame_callback_;
    Mp4AudioCallback audio_callback_;
};

#endif // MP4_PLAYER_H
