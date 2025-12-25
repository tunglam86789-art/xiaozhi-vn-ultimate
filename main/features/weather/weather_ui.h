#ifndef WEATHER_UI_H
#define WEATHER_UI_H

#include "weather_model.h"
#include "weather_config.h"
#include "lvgl_display.h"
#if HAVE_LVGL
#include <lvgl.h>
#endif

#include <string>

class WeatherUI {
public:
    WeatherUI();
    ~WeatherUI();

    // Initialize UI elements
    void SetupIdleUI(lv_obj_t* parent, int screen_width, int screen_height);

    // Show weather card with info
    void ShowIdleCard(const IdleCardInfo& info);

    // Hide weather card
    void HideIdleCard();

    // Update idle display with weather data
    void UpdateIdleDisplay(const WeatherInfo& weather_info);

    // Get weather icon from code
    static const char* GetWeatherIcon(const std::string& code);

    // Check if UI is initialized
    bool IsInitialized() const { return idle_panel_ != nullptr; }

private:
    // UI elements
    lv_obj_t* idle_panel_;
    lv_obj_t* idle_time_label_;
    lv_obj_t* idle_date_label_;
    lv_obj_t* idle_temp_label_;
    lv_obj_t* idle_icon_label_;
    lv_obj_t* idle_city_label_;
    lv_obj_t* idle_detail_label_;

    int screen_width_;
    int screen_height_;
};

#endif // WEATHER_UI_H
