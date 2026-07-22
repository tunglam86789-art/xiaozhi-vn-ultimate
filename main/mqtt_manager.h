#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <string>
#include "mqtt_client.h"

class MqttManager {
public:
    static MqttManager& GetInstance();

    struct ElectricityData {
    float power_kw = 0.0f;
    float today_kwh = 0.0f;
    float month_kwh = 0.0f;
    float today_cost = 0.0f;
    float month_cost = 0.0f;
    float forecast_cost = 0.0f;
    bool valid = false;
};

ElectricityData GetElectricityData() const;

    void Start();
    void Publish(const std::string& topic,
                 const std::string& payload,
                 int qos = 0,
                 bool retain = false);

    bool IsConnected() const {
        return connected_;
    }

private:
ElectricityData electricity_data_;
    MqttManager() = default;
    ~MqttManager() = default;

    MqttManager(const MqttManager&) = delete;
    MqttManager& operator=(const MqttManager&) = delete;

    static void EventHandler(void* handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void* event_data);

    void HandleEvent(esp_mqtt_event_handle_t event);

    esp_mqtt_client_handle_t client_ = nullptr;
    bool connected_ = false;
};

#endif