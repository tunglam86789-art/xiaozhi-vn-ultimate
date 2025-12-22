
#include "esp_wifi.h"
#include "esp_freertos_hooks.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/rtc_io.h"


static const char *TAG = "PM";


// 注意：ESP32只有特定GPIO可以用作深度睡眠唤醒源：0, 2, 4, 12-15, 25-27, 32-39
#define BUTTON_PIN     GPIO_NUM_0 //GPIO_NUM_38 //GPIO_NUM_0 


void pm_wakeup(void)
{
    // 检查是否从Deep-Sleep唤醒
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Wake up from EXT0 (GPIO%d)", BUTTON_PIN);
    } else if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Wake up from EXT1");
    } else {
        ESP_LOGI(TAG, "Power-on or reset");
    }
}

void pm_low_power_shutdown(void)
{  
    // 配置RTC GPIO作为唤醒源
    rtc_gpio_init(BUTTON_PIN);
    rtc_gpio_set_direction(BUTTON_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BUTTON_PIN);
    rtc_gpio_pulldown_dis(BUTTON_PIN);

    esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);
    
    ESP_LOGI(TAG, "进入深度睡眠，将通过GPIO%d低电平唤醒", BUTTON_PIN);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start(); // 进入Deep-Sleep
}
