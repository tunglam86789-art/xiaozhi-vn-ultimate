#include "mqtt_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"

#include <cstdio>
#include <string>
static const char* TAG = "MqttManager";
static constexpr const char* MQTT_BROKER_URI =
    "mqtt://192.168.50.5:1883";

MqttManager& MqttManager::GetInstance() {
    static MqttManager instance;
    return instance;
}

void MqttManager::Start() {
    if (client_ != nullptr) {
        ESP_LOGW(TAG, "MQTT client already started");
        return;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char client_id[40];
    std::snprintf(
        client_id,
        sizeof(client_id),
        "xiaozhi-%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]
    );

    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = MQTT_BROKER_URI;
    config.credentials.client_id = client_id;
    config.session.keepalive = 30;

    client_ = esp_mqtt_client_init(&config);
    if (client_ == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return;
    }

    esp_mqtt_client_register_event(
        client_,
        MQTT_EVENT_ANY,
        &MqttManager::EventHandler,
        this
    );

    esp_err_t err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_mqtt_client_start failed: %s",
            esp_err_to_name(err)
        );

        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Connecting to %s", MQTT_BROKER_URI);
}

void MqttManager::Publish(const std::string& topic,
                          const std::string& payload,
                          int qos,
                          bool retain) {
    if (client_ == nullptr || !connected_) {
        ESP_LOGW(TAG, "Publish ignored: MQTT disconnected");
        return;
    }

    int message_id = esp_mqtt_client_publish(
        client_,
        topic.c_str(),
        payload.c_str(),
        static_cast<int>(payload.size()),
        qos,
        retain ? 1 : 0
    );

    if (message_id < 0) {
        ESP_LOGE(TAG, "Publish failed: %s", topic.c_str());
    }
}

void MqttManager::EventHandler(void* handler_args,
                               esp_event_base_t,
                               int32_t,
                               void* event_data) {
    auto* manager = static_cast<MqttManager*>(handler_args);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);

    if (manager != nullptr && event != nullptr) {
        manager->HandleEvent(event);
    }
}

void MqttManager::HandleEvent(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
case MQTT_EVENT_CONNECTED: {
    connected_ = true;
    ESP_LOGI(TAG, "MQTT connected");

    // Báo trạng thái ESP32 đang online
    esp_mqtt_client_publish(
        client_,
        "baominh/esp32/status",
        "online",
        0,
        1,
        1
    );

    // Nhận lệnh điều khiển TV
    int command_msg_id = esp_mqtt_client_subscribe(
        client_,
        "baominh/tv/command",
        1
    );

    ESP_LOGI(
        TAG,
        "Command subscribe requested: baominh/tv/command, msg_id=%d",
        command_msg_id
    );

    // Nhận dữ liệu điện từ Home Assistant
    int electricity_msg_id = esp_mqtt_client_subscribe(
        client_,
        "baominh/esp32/electricity",
        1
    );

    ESP_LOGI(
        TAG,
        "Electricity subscribe requested: baominh/esp32/electricity, msg_id=%d",
        electricity_msg_id
    );

    break;
}

        case MQTT_EVENT_DISCONNECTED:
            connected_ = false;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_DATA: {
    std::string topic(event->topic, event->topic_len);
    std::string payload(event->data, event->data_len);

    ESP_LOGI(
        TAG,
        "Received topic=%s data=%s",
        topic.c_str(),
        payload.c_str()
    );

  if (topic == "baominh/esp32/electricity") {
    cJSON* root = cJSON_Parse(payload.c_str());

    if (root == nullptr) {
        ESP_LOGE(TAG, "Invalid electricity JSON");
        break;
    }

    auto read_number = [root](const char* key, float old_value) {
        cJSON* item = cJSON_GetObjectItem(root, key);

        if (cJSON_IsNumber(item)) {
            return static_cast<float>(item->valuedouble);
        }

        return old_value;
    };

    electricity_data_.power_kw =
        read_number("power_kw", electricity_data_.power_kw);

    electricity_data_.today_kwh =
        read_number("today_kwh", electricity_data_.today_kwh);

    electricity_data_.month_kwh =
        read_number("month_kwh", electricity_data_.month_kwh);

    electricity_data_.today_cost =
        read_number("today_cost", electricity_data_.today_cost);

    electricity_data_.month_cost =
        read_number("month_cost", electricity_data_.month_cost);

    electricity_data_.forecast_cost =
        read_number("forecast_cost", electricity_data_.forecast_cost);

    electricity_data_.valid = true;

    ESP_LOGI(
        TAG,
        "Electricity saved: power=%.2f kW, today=%.2f kWh, month=%.2f kWh, month_cost=%.0f VND",
        electricity_data_.power_kw,
        electricity_data_.today_kwh,
        electricity_data_.month_kwh,
        electricity_data_.month_cost
    );

    cJSON_Delete(root);
}

    break;
}

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

MqttManager::ElectricityData MqttManager::GetElectricityData() const {
    return electricity_data_;
}