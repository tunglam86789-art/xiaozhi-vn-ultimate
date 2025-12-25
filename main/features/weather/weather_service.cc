#include "weather_service.h"
#include "settings.h"
#include "wifi_station.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cmath>

#define TAG "WeatherService"

WeatherService::WeatherService() 
    : api_key_(OPEN_WEATHERMAP_API_KEY_DEFAULT)
    , last_update_time_(0) {
    weather_info_.valid = false;
}

bool WeatherService::NeedsUpdate() const {
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return (current_time - last_update_time_) >= WEATHER_UPDATE_INTERVAL_MS;
}

std::string WeatherService::UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (std::string::const_iterator i = value.begin(), n = value.end(); i != n; ++i) {
        std::string::value_type c = (*i);
        
        // Keep alphanumeric and other accepted characters intact
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
            continue;
        }
        
        // Any other characters are percent-encoded
        escaped << std::uppercase;
        escaped << '%' << std::setw(2) << int((unsigned char)c);
        escaped << std::nouppercase;
    }

    return escaped.str();
}

std::string WeatherService::CapitalizeWords(std::string str) {
    bool newWord = true;
    for (size_t i = 0; i < str.length(); ++i) {
        if (std::isspace(str[i])) {
            newWord = true;
        } else if (newWord) {
            str[i] = std::toupper(str[i]);
            newWord = false;
        }
    }
    return str;
}

std::string WeatherService::GetCityFromIP() {
    std::string detected_city = "";
    
    // Check WiFi connection
    if (!WifiStation::GetInstance().IsConnected()) {
        ESP_LOGE(TAG, "GetCityFromIP: No WiFi connection");
        return "";
    }

    esp_http_client_config_t config = {};
    config.url = IP_LOCATION_API_ENDPOINT;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = WEATHER_HTTP_TIMEOUT_MS;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for IP detection");
        return "";
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "IP-Who-Is Status: %d, Content-Length: %d", status_code, content_length);

        if (status_code == 200) {
            std::string body;
            char buffer[512];
            int read_len = 0;
            while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
                body.append(buffer, read_len);
            }
            
            ESP_LOGI(TAG, "IP-Who-Is Response: %s", body.c_str());

            if (!body.empty()) {
                cJSON* root = cJSON_Parse(body.c_str());
                if (root) {
                    cJSON* success = cJSON_GetObjectItem(root, "success");
                    
                    if (cJSON_IsBool(success) && cJSON_IsTrue(success)) {
                        cJSON* city_json = cJSON_GetObjectItem(root, "city");
                        if (cJSON_IsString(city_json)) {
                            detected_city = city_json->valuestring;
                            ESP_LOGI(TAG, "Auto-detected City success: %s", detected_city.c_str());
                        }
                    } else {
                        ESP_LOGW(TAG, "IP-Who-Is returned success=false");
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON");
                }
            }
        } else {
            ESP_LOGW(TAG, "HTTP Error: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Failed to connect: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return detected_city;
}

bool WeatherService::FetchWeatherData() {
#if !CONFIG_ENABLE_WEATHER_FEATURE
    ESP_LOGW(TAG, "Weather feature is disabled");
    return false;
#endif

    // Wait for WiFi connection
    int wait_retries = 0;
    while (!WifiStation::GetInstance().IsConnected() && wait_retries < 20) {
        ESP_LOGI(TAG, "Waiting for WiFi before fetching weather...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait_retries++;
    }

    if (!WifiStation::GetInstance().IsConnected()) {
        ESP_LOGE(TAG, "WiFi not connected, skipping weather update.");
        return false;
    }

    // Get configuration
    Settings weather_settings("weather", false);
    std::string city = city_.empty() ? weather_settings.GetString("city", "") : city_;
    std::string api_key = api_key_.empty() ? weather_settings.GetString("api_key", "") : api_key_;

    if (api_key.empty()) {
        api_key = OPEN_WEATHERMAP_API_KEY_DEFAULT;
    }
    
    // Auto-detect city if not set
    if (city.empty() || city == "auto") {
        ESP_LOGI(TAG, "City not set or 'auto', detecting via IP...");
        
        for (int i = 0; i < 2; i++) {
            std::string auto_city = GetCityFromIP();
            if (!auto_city.empty()) {
                city = auto_city;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (city.empty() || city == "auto") {
            ESP_LOGW(TAG, "Failed to detect IP location, fallback to Hanoi");
            city = "Hanoi";
        }
    }

    // Call OpenWeatherMap API
    std::string url = std::string(WEATHER_API_ENDPOINT) + "?q=" + UrlEncode(city) +
                      "&appid=" + api_key + "&units=metric&lang=en";

    ESP_LOGI(TAG, "Fetching weather from: %s", url.c_str());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = WEATHER_HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    bool success = false;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        
        if (status_code == 200) {
            std::string body;
            char buffer[512];
            int read_len = 0;
            while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
                body.append(buffer, read_len);
            }
            
            cJSON* root = cJSON_Parse(body.c_str());
            if (root) {
                do {
                    cJSON* name = cJSON_GetObjectItem(root, "name");
                    cJSON* main_obj = cJSON_GetObjectItem(root, "main");
                    cJSON* weather_arr = cJSON_GetObjectItem(root, "weather");

                    if (!cJSON_IsString(name) || !cJSON_IsObject(main_obj) || 
                        !cJSON_IsArray(weather_arr) || cJSON_GetArraySize(weather_arr) == 0) {
                        break;
                    }

                    cJSON* temp = cJSON_GetObjectItem(main_obj, "temp");
                    cJSON* humidity = cJSON_GetObjectItem(main_obj, "humidity");
                    cJSON* feels_like = cJSON_GetObjectItem(main_obj, "feels_like");
                    cJSON* pressure = cJSON_GetObjectItem(main_obj, "pressure");
                    cJSON* wind_obj = cJSON_GetObjectItem(root, "wind");
                    cJSON* wind_speed = cJSON_IsObject(wind_obj) ? cJSON_GetObjectItem(wind_obj, "speed") : nullptr;
                    cJSON* w0 = cJSON_GetArrayItem(weather_arr, 0);
                    cJSON* icon = cJSON_GetObjectItem(w0, "icon");
                    cJSON* desc = cJSON_GetObjectItem(w0, "description");

                    weather_info_.city = name->valuestring;
                    if (cJSON_IsNumber(temp)) weather_info_.temp = (float)temp->valuedouble;
                    if (cJSON_IsNumber(humidity)) weather_info_.humidity = humidity->valueint;
                    if (cJSON_IsNumber(feels_like)) weather_info_.feels_like = (float)feels_like->valuedouble;
                    if (cJSON_IsNumber(pressure)) weather_info_.pressure = pressure->valueint;
                    if (cJSON_IsNumber(wind_speed)) weather_info_.wind_speed = (float)wind_speed->valuedouble;
                    if (cJSON_IsString(icon)) weather_info_.icon_code = icon->valuestring;
                    if (cJSON_IsString(desc)) weather_info_.description = CapitalizeWords(desc->valuestring);
                    
                    weather_info_.valid = true;
                    success = true;
                    last_update_time_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    ESP_LOGI(TAG, "Weather Updated: %s, %.1f C", weather_info_.city.c_str(), weather_info_.temp);

                } while (0);
                cJSON_Delete(root);
            }
        } else {
            ESP_LOGE(TAG, "OpenWeatherMap HTTP Error: %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Connection failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    
    return success;
}
