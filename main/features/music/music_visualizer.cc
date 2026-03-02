/**
 * @file music_visualizer.cc
 * @brief Self-contained music spectrum visualizer + player info overlay.
 *
 * Contributors: Xiaozhi AI-IoT Vietnam Team
 */

#include "music_visualizer.h"
#include "esp32_sd_music.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <font_awesome.h>
#include <cstring>
#include <cmath>
#include <cctype>
#include <ctime>
#include <algorithm>

static const char* TAG = "MusicVisualizer";

namespace music {

// ===================================================================
// Lifecycle
// ===================================================================

MusicVisualizer::~MusicVisualizer() {
    Stop();
}

bool MusicVisualizer::Start(const VisualizerConfig& cfg, const std::string& music_info) {
    if (spectrum_mgr_ && spectrum_mgr_->IsRunning()) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    config_ = cfg;
    if (!music_info.empty()) music_info_ = music_info;

    // Build spectrum config from caller-provided canvas rect
    spectrum::SpectrumConfig scfg;
    scfg.canvas_x       = cfg.canvas_x;
    scfg.canvas_y       = cfg.canvas_y;
    scfg.canvas_width   = cfg.canvas_width;
    scfg.canvas_height  = cfg.canvas_height;
    scfg.lcd_width      = cfg.canvas_width;
    scfg.lcd_height     = cfg.canvas_height;
    scfg.status_bar_h   = cfg.status_bar_h;
    scfg.bar_max_height = scfg.canvas_height / 2;
    scfg.fft_size       = cfg.fft_size;
    scfg.bar_count      = cfg.bar_count;

    spectrum_mgr_ = std::make_unique<spectrum::SpectrumManager>(scfg);

    // Allocate PCM input buffer BEFORE starting the task
    if (!spectrum_mgr_->AllocateAudioBuffer(cfg.audio_buf_size)) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer (%u bytes)",
                 static_cast<unsigned>(cfg.audio_buf_size));
        spectrum_mgr_.reset();
        return false;
    }

    // Periodic callback for music UI updates (every 1 s, LVGL lock held by caller)
    spectrum_mgr_->SetPeriodicCallback([this]() { UpdateMusicUI(); }, 1000);

    // Start spectrum (canvas + task)
    if (!spectrum_mgr_->Start()) {
        ESP_LOGE(TAG, "Failed to start spectrum manager");
        spectrum_mgr_.reset();
        return false;
    }

    // Build music overlay inside LVGL lock
    if (lvgl_port_lock(3000)) {
        if (overlay_cb_) overlay_cb_(true);   // hide main UI
        BuildMusicUI();
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Started (%dx%d at %d,%d)", cfg.canvas_width, cfg.canvas_height, cfg.canvas_x, cfg.canvas_y);
    return true;
}

void MusicVisualizer::Stop() {
    if (spectrum_mgr_) {
        spectrum_mgr_->Stop();
    }

    if (lvgl_port_lock(3000)) {
        DestroyMusicUI();
        if (overlay_cb_) overlay_cb_(false);  // restore main UI
        lvgl_port_unlock();
    }

    spectrum_mgr_.reset();
    ESP_LOGI(TAG, "Stopped");
}

bool MusicVisualizer::IsRunning() const {
    return spectrum_mgr_ && spectrum_mgr_->IsRunning();
}

// ===================================================================
// Audio feed (thread-safe)
// ===================================================================

int16_t* MusicVisualizer::AllocateAudioBuffer(size_t sample_count) {
    if (!spectrum_mgr_) return nullptr;
    return spectrum_mgr_->AllocateAudioBuffer(sample_count);
}

void MusicVisualizer::FeedAudioData(const int16_t* data, size_t sample_count) {
    if (spectrum_mgr_) {
        spectrum_mgr_->FeedAudioData(data, sample_count);
    }
}

void MusicVisualizer::ReleaseAudioBuffer() {
    if (spectrum_mgr_) {
        spectrum_mgr_->ReleaseAudioBuffer();
    }
}

// ===================================================================
// Music info
// ===================================================================

void MusicVisualizer::SetMusicInfo(const char* info) {
    if (info)
        music_info_ = info;
    else
        music_info_.clear();

    // If running, update labels live (caller must hold LVGL lock)
    if (!IsRunning() || !music_root_) return;

    std::string line1, line2;
    size_t pos = music_info_.find('\n');
    if (pos != std::string::npos) {
        line1 = music_info_.substr(0, pos);
        line2 = music_info_.substr(pos + 1);
    } else {
        line1 = music_info_;
    }

    int cw = spectrum_mgr_ ? spectrum_mgr_->GetConfig().canvas_width : config_.canvas_width;

    if (music_title_label_ && lv_obj_is_valid(music_title_label_)) {
        lv_label_set_text(music_title_label_, line1.c_str());
        lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(music_title_label_, cw - 40);
    }
    if (music_subinfo_label_ && lv_obj_is_valid(music_subinfo_label_)) {
        lv_label_set_text(music_subinfo_label_, line2.c_str());
        lv_label_set_long_mode(music_subinfo_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(music_subinfo_label_, cw - 40);
    }
}

SourceType MusicVisualizer::DetectSource() const {
    std::string s = music_info_;
    for (auto& c : s) c = (char)tolower((unsigned char)c);

    if (s.find("radio") != std::string::npos || s.find("fm") != std::string::npos)
        return SourceType::RADIO;
    if (s.rfind("ONLINE:", 0) == 0) return SourceType::ONLINE;
    if (s.find("online") != std::string::npos || s.find("http") != std::string::npos ||
        s.find("rtmp") != std::string::npos   || s.find("m3u") != std::string::npos)
        return SourceType::ONLINE;

    Esp32SdMusic* sd = sd_getter_ ? sd_getter_() : nullptr;
    if (sd && sd->getState() == Esp32SdMusic::PlayerState::Playing)
        return SourceType::SD_CARD;

    return SourceType::NONE;
}

// ===================================================================
// Music UI — Build
// ===================================================================

void MusicVisualizer::BuildMusicUI() {
    if (!spectrum_mgr_ || !spectrum_mgr_->GetRenderer()) return;

    lv_obj_t* canvas = spectrum_mgr_->GetRenderer()->GetCanvas();
    if (!canvas) return;

    const int w = spectrum_mgr_->GetConfig().canvas_width;
    const int h = spectrum_mgr_->GetConfig().canvas_height;

    Esp32SdMusic* sd_player = sd_getter_ ? sd_getter_() : nullptr;
    bool sd_playing = sd_player &&
                      sd_player->getState() == Esp32SdMusic::PlayerState::Playing;
    SourceType source = DetectSource();

    lv_color_t accent;
    const char* icon_sym;
    switch (source) {
        case SourceType::SD_CARD:
            accent   = lv_color_hex(0x00FFC2);
            icon_sym = LV_SYMBOL_SD_CARD;
            break;
        case SourceType::RADIO:
            accent   = lv_color_hex(0xFF9E40);
            icon_sym = LV_SYMBOL_VOLUME_MAX;
            break;
        case SourceType::ONLINE:
            accent   = lv_color_hex(0x00D9FF);
            icon_sym = LV_SYMBOL_AUDIO;
            break;
        default:
            accent   = lv_color_hex(0xFFFFFF);
            icon_sym = LV_SYMBOL_AUDIO;
            break;
    }

    if (source == SourceType::NONE && !sd_playing) return;

    // Get fonts via callback
    const lv_font_t* text_font = &lv_font_montserrat_14;
    const lv_font_t* icon_font = &lv_font_montserrat_14;
    if (font_provider_) font_provider_(&text_font, &icon_font);

    const int pad_side = (int)(w * 0.04f);
    const int pad_top  = (int)(h * 0.05f);

    // Root container
    music_root_ = lv_obj_create(canvas);
    lv_obj_remove_style_all(music_root_);
    lv_obj_set_size(music_root_, w, h);
    lv_obj_set_style_bg_opa(music_root_, LV_OPA_TRANSP, 0);

    // Gradient overlay (top 35 %)
    lv_obj_t* overlay = lv_obj_create(music_root_);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, w, (int)(h * 0.35f));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(overlay, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(overlay, 200, 0);
    lv_obj_set_style_bg_main_stop(overlay, 0, 0);
    lv_obj_set_style_bg_grad_stop(overlay, 255, 0);

    // Icon
    lv_obj_t* icon = lv_label_create(music_root_);
    lv_obj_set_style_text_font(icon, icon_font, 0);
    lv_obj_set_style_text_color(icon, accent, 0);
    lv_label_set_text(icon, icon_sym);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, pad_side, pad_top);

    // Title + sub-info strings
    std::string title_str, sub_str;
    bool show_progress = false;

    if (source == SourceType::SD_CARD && sd_playing && sd_player) {
        title_str = sd_player->getCurrentTrack();
        if (title_str.empty()) title_str = "Unknown Track";
        int br = sd_player->getBitrate();
        char buf[32];
        snprintf(buf, sizeof(buf), "%d kbps / MP3", br / 1000);
        sub_str = buf;
        show_progress = true;
    } else {
        std::string l1, l2;
        size_t p = music_info_.find('\n');
        if (p != std::string::npos) { l1 = music_info_.substr(0, p); l2 = music_info_.substr(p + 1); }
        else l1 = music_info_;
        title_str = l1.empty()
                    ? (source == SourceType::ONLINE ? "Music Online" : "FM Radio")
                    : l1;
        if (source == SourceType::ONLINE) sub_str = !l2.empty() ? l2 : "Playing...";
        else if (source == SourceType::RADIO) sub_str = !l2.empty() ? l2 : "Live Broadcast";
    }

    int icon_w = 30;
    int text_w = w - (pad_side + icon_w + pad_side) - pad_side;

    // Title
    lv_obj_t* title = lv_label_create(music_root_);
    lv_obj_set_style_text_font(title, text_font, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, text_w);
    lv_label_set_text(title, title_str.c_str());
    lv_obj_align_to(title, icon, LV_ALIGN_OUT_RIGHT_TOP, pad_side, 0);
    music_title_label_ = title;

    // Sub-info
    lv_obj_t* sub = lv_label_create(music_root_);
    lv_obj_set_style_text_font(sub, text_font, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(sub, sub_str.c_str());
    lv_label_set_long_mode(sub, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(sub, w - 40);
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    music_subinfo_label_ = sub;

    // Progress bar (SD card only)
    if (show_progress && sd_player) {
        int64_t pos_ms = sd_player->getCurrentPositionMs();
        int64_t dur_ms = sd_player->getDurationMs();

        lv_obj_t* bar = lv_bar_create(music_root_);
        lv_obj_set_size(bar, w - (pad_side * 2), 4);
        lv_obj_align_to(bar, sub, LV_ALIGN_OUT_BOTTOM_LEFT, -icon_w - pad_side, 12);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x303030), LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, dur_ms);
        lv_bar_set_value(bar, pos_ms, LV_ANIM_OFF);
        music_bar_ = bar;

        lv_obj_t* t_curr = lv_label_create(music_root_);
        lv_obj_set_style_text_font(t_curr, text_font, 0);
        lv_obj_set_style_text_color(t_curr, accent, 0);
        lv_label_set_text(t_curr, sd_player->getCurrentTimeString().c_str());
        lv_obj_align_to(t_curr, bar, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
        music_time_left_ = t_curr;

        lv_obj_t* t_dur = lv_label_create(music_root_);
        lv_obj_set_style_text_font(t_dur, text_font, 0);
        lv_obj_set_style_text_color(t_dur, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(t_dur, sd_player->getDurationString().c_str());
        lv_obj_align_to(t_dur, bar, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 6);
        music_time_remain_ = t_dur;

        // Next track
        auto tracks = sd_player->listTracks();
        std::string cur_path = sd_player->getCurrentTrackPath();
        int idx = -1;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].path == cur_path) idx = (int)i;
        std::string next_txt = "End of playlist";
        if (idx >= 0 && idx < (int)tracks.size() - 1)
            next_txt = tracks[idx + 1].name;
        else if (!tracks.empty())
            next_txt = tracks[0].name;

        lv_obj_t* nl = lv_label_create(music_root_);
        lv_obj_set_style_text_font(nl, text_font, 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(0x707070), 0);
        char nb[128];
        snprintf(nb, sizeof(nb), "Next: %s", next_txt.c_str());
        lv_label_set_text(nl, nb);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(nl, w - pad_side * 2);
        lv_obj_align_to(nl, t_curr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
        music_next_line_ = nl;
    } else {
        music_bar_         = nullptr;
        music_time_left_   = nullptr;
        music_time_remain_ = nullptr;
        music_next_line_   = nullptr;
        music_time_total_  = nullptr;
    }

    ESP_LOGI(TAG, "Music UI built");
}

// ===================================================================
// Music UI — Destroy
// ===================================================================

void MusicVisualizer::DestroyMusicUI() {
    if (music_root_ && lv_obj_is_valid(music_root_)) {
        lv_obj_del(music_root_);
    }
    music_root_          = nullptr;
    music_title_label_   = nullptr;
    music_date_label_    = nullptr;
    music_bar_           = nullptr;
    music_time_left_     = nullptr;
    music_time_total_    = nullptr;
    music_time_remain_   = nullptr;
    music_subinfo_label_ = nullptr;
    music_next_line_     = nullptr;
}

// ===================================================================
// Music UI — Periodic update (called with LVGL lock held)
// ===================================================================

void MusicVisualizer::UpdateMusicUI() {
    Esp32SdMusic* sd = sd_getter_ ? sd_getter_() : nullptr;
    if (!sd) return;
    if (!music_root_ || !lv_obj_is_valid(music_root_)) return;
    if (!music_bar_  || !lv_obj_is_valid(music_bar_))   return;

    lv_bar_set_range(music_bar_, 0, sd->getDurationMs());
    lv_bar_set_value(music_bar_, sd->getCurrentPositionMs(), LV_ANIM_OFF);

    if (music_time_left_ && lv_obj_is_valid(music_time_left_))
        lv_label_set_text(music_time_left_, sd->getCurrentTimeString().c_str());

    if (music_time_remain_ && lv_obj_is_valid(music_time_remain_)) {
        int64_t rem = sd->getDurationMs() - sd->getCurrentPositionMs();
        if (rem < 0) rem = 0;
        lv_label_set_text(music_time_remain_, MsToTimeString(rem).c_str());
    }

    if (music_title_label_ && lv_obj_is_valid(music_title_label_)) {
        std::string t = sd->getCurrentTrack();
        if (!t.empty()) lv_label_set_text(music_title_label_, t.c_str());
    }

    if (music_date_label_ && lv_obj_is_valid(music_date_label_)) {
        time_t now = time(nullptr);
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[32];
        strftime(buf, sizeof(buf), "%d-%m-%Y", &ti);
        lv_label_set_text(music_date_label_, buf);
    }

    if (music_subinfo_label_ && lv_obj_is_valid(music_subinfo_label_)) {
        int br = sd->getBitrate();
        if (br > 1000) br /= 1000;
        char st[64];
        snprintf(st, sizeof(st), "%d kbps  •  %s", br, sd->getDurationString().c_str());
        lv_label_set_text(music_subinfo_label_, st);
        lv_label_set_long_mode(music_subinfo_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        int cw = spectrum_mgr_ ? spectrum_mgr_->GetConfig().canvas_width : config_.canvas_width;
        lv_obj_set_width(music_subinfo_label_, cw - 40);
    }

    if (music_next_line_ && lv_obj_is_valid(music_next_line_)) {
        auto list = sd->listTracks();
        std::string cp = sd->getCurrentTrackPath();
        int cur = 0;
        for (int i = 0; i < (int)list.size(); i++)
            if (list[i].path == cp) { cur = i; break; }
        int total = (int)list.size();
        int next  = (cur + 1) % total;
        std::string nt = (next < total) ? list[next].name : "No next track";
        std::string tip = "Next: " + nt;
        lv_label_set_text(music_next_line_, tip.c_str());
    }
}

// ===================================================================
// Utility
// ===================================================================

std::string MusicVisualizer::MsToTimeString(int64_t ms) {
    if (ms < 0) ms = 0;
    int ts  = (int)(ms / 1000);
    int s   = ts % 60;
    int m   = (ts / 60) % 60;
    int h   = ts / 3600;
    char buf[32];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    else
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return buf;
}

}  // namespace music
