#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

// ---[DienBien Mod]--[STRUCTS M?I CHO MÀN HÌNH CH? TH?I TI?T] ---
struct IdleCardInfo {
    std::string city;
    std::string time_text;
    std::string date_text;
    std::string day_text;
    std::string temperature_text;
    std::string humidity_text;
    std::string description_text;
    std::string feels_like_text;
    std::string wind_text;
    std::string pressure_text;
    const char* icon = nullptr;
};

struct WeatherInfo {
    std::string city;
    std::string description;
    std::string icon_code;
    float temp = 0.0f;
    int humidity = 0;
    float feels_like = 0.0f;
    int pressure = 0;
    float wind_speed = 0.0f;
    bool valid = false;
};
// --------------------------------------

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
private:
    std::string name_;
};

class Display {
public:
    enum class DisplaySourceType {
        NONE = 0,
        SD_CARD,
        ONLINE,
        RADIO
    };

public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void SetMusicInfo(const char* song_name);
    virtual DisplaySourceType DetectSourceFromInfo() { return DisplaySourceType::NONE; }
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);

    // For FFT display
    virtual void StartFFT() {}
    virtual void StopFFT() {}
    virtual void FeedAudioDataFFT(int16_t* data, size_t sample_count) {};
    virtual int16_t* MakeAudioBuffFFT(size_t sample_count) { return nullptr; };
    virtual void ReleaseAudioBuffFFT(int16_t* buffer = nullptr) {};

    // For QR code display
    virtual void ClearQRCode() {}
    virtual bool QRCodeIsSupported() { return false; }
    virtual void DisplayQRCode(const uint8_t* qrcode, const char* text = nullptr) {}
    virtual void SetIpAddress(const std::string& ip_address) {}

    // For rotation display
    virtual bool SetRotation(int rotation_degree, bool save_setting) { return false; }
    
    // --- [HÀM M?I CHO IDLE SCREEN] ---
    virtual void ShowIdleCard(const IdleCardInfo& info) {}
    virtual void HideIdleCard() {}

    inline int width() const { return width_; }
    inline int height() const { return height_; }

protected:
    int width_ = 0;
    int height_ = 0;

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }

private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
