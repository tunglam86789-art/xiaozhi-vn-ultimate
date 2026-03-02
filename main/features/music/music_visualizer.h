/**
 * @file music_visualizer.h
 * @brief Self-contained music spectrum visualizer + player info overlay.
 *
 * Owns:
 *   • SpectrumManager  (FFT + LVGL canvas bars)
 *   • Music UI overlay (title, sub-info, progress bar, next track)
 *
 * Isolation contract:
 *   • Does NOT depend on Display, LcdDisplay, or any display class.
 *   • Uses a simple callback (`OverlayCallback`) to tell the host
 *     whether the media overlay should be active or not.
 *   • Accesses the SD-music player through an abstract getter callback
 *     so it never directly includes Application.
 *
 * Thread safety:
 *   • Audio feed methods are thread-safe (called from streaming task).
 *   • All LVGL calls acquire `lvgl_port_lock` internally.
 *
 * Contributors: Xiaozhi AI-IoT Vietnam Team
 */
#pragma once

#include "spectrum_manager.h"

#include <lvgl.h>
#include <functional>
#include <string>
#include <memory>

// Forward declare — we only use a pointer, no include needed
class Esp32SdMusic;

namespace music {

/**
 * @brief Source type for the music currently playing.
 */
enum class SourceType {
    NONE = 0,
    SD_CARD,
    ONLINE,
    RADIO,
};

/**
 * @brief Configuration for the visualizer.
 */
struct VisualizerConfig {
    int screen_width      = 320;
    int screen_height     = 240;
    int status_bar_h      = 0;        ///< Height of status bar (canvas starts below)
    int fft_size          = 512;
    int bar_count         = 40;
    size_t audio_buf_size = 4608;    ///< PCM buffer size in bytes (must match pipeline)
};

/**
 * @brief Self-contained music spectrum visualizer.
 *
 * Usage:
 * @code
 *   music::MusicVisualizer viz;
 *   viz.SetOverlayCallback([&](bool active) { display->SetMediaOverlayActive(active); });
 *   viz.SetSdPlayerGetter([]() -> Esp32SdMusic* { return app.GetSdMusic(); });
 *   viz.SetFontProvider([&](const lv_font_t** text, const lv_font_t** icon) { ... });
 *   viz.Start(config);
 *   // feed audio...
 *   viz.Stop();
 * @endcode
 */
class MusicVisualizer {
public:
    /// Callback to show/hide the media overlay on the host display.
    using OverlayCallback   = std::function<void(bool active)>;
    /// Callback to retrieve the SD music player pointer.
    using SdPlayerGetter    = std::function<Esp32SdMusic*()>;
    /// Callback to retrieve fonts from the display theme.
    using FontProvider      = std::function<void(const lv_font_t** text_font,
                                                 const lv_font_t** icon_font)>;

    MusicVisualizer() = default;
    ~MusicVisualizer();

    // Non-copyable
    MusicVisualizer(const MusicVisualizer&) = delete;
    MusicVisualizer& operator=(const MusicVisualizer&) = delete;

    // ─── Callback registration (call before Start) ────────────────

    /** Register callback to show/hide the media overlay. */
    void SetOverlayCallback(OverlayCallback cb) { overlay_cb_ = std::move(cb); }

    /** Register callback to get the SD music player pointer. */
    void SetSdPlayerGetter(SdPlayerGetter cb) { sd_getter_ = std::move(cb); }

    /** Register callback to get theme fonts. */
    void SetFontProvider(FontProvider cb) { font_provider_ = std::move(cb); }

    // ─── Lifecycle ────────────────────────────────────────────────

    /**
     * @brief Start spectrum rendering + music UI.
     *
     * Creates the spectrum canvas, builds the music info overlay,
     * and spawns the background FFT task.
     */
    bool Start(const VisualizerConfig& cfg, const std::string& music_info = "");

    /** Stop spectrum + tear down music UI. */
    void Stop();

    /** @return true if currently running. */
    bool IsRunning() const;

    // ─── Audio data feed (thread-safe) ────────────────────────────

    /** Allocate a PCM buffer for the audio pipeline. */
    int16_t* AllocateAudioBuffer(size_t sample_count);

    /** Feed PCM data to the FFT analyzer. */
    void FeedAudioData(const int16_t* data, size_t sample_count);

    /** Release the PCM buffer. */
    void ReleaseAudioBuffer();

    // ─── Music info ───────────────────────────────────────────────

    /**
     * @brief Update the displayed music info string.
     *
     * If the visualizer is running the labels are updated live.
     * Must be called with the LVGL lock held.
     */
    void SetMusicInfo(const char* info);

    /** Detect source type from the current music_info_ string. */
    SourceType DetectSource() const;

private:
    // ─── Music UI helpers ─────────────────────────────────────────
    void BuildMusicUI();
    void DestroyMusicUI();
    void UpdateMusicUI();

    static std::string MsToTimeString(int64_t ms);

    // ─── Callbacks ────────────────────────────────────────────────
    OverlayCallback overlay_cb_;
    SdPlayerGetter  sd_getter_;
    FontProvider    font_provider_;

    // ─── Spectrum engine ──────────────────────────────────────────
    std::unique_ptr<spectrum::SpectrumManager> spectrum_mgr_;

    // ─── Music state ──────────────────────────────────────────────
    std::string music_info_;
    VisualizerConfig config_{};

    // ─── LVGL music UI widgets ────────────────────────────────────
    lv_obj_t* music_root_          = nullptr;
    lv_obj_t* music_title_label_   = nullptr;
    lv_obj_t* music_date_label_    = nullptr;
    lv_obj_t* music_bar_           = nullptr;
    lv_obj_t* music_time_left_     = nullptr;
    lv_obj_t* music_time_total_    = nullptr;
    lv_obj_t* music_subinfo_label_ = nullptr;
    lv_obj_t* music_time_remain_   = nullptr;
    lv_obj_t* music_next_line_     = nullptr;
};

}  // namespace music
