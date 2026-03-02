/**
 * @file qrcode_display.h
 * @brief Independent QR code display component.
 *
 * Renders a QR code on its own LVGL canvas, fully isolated from
 * any specific Display subclass.  Caller provides LVGL lock/unlock.
 *
 * Contributors: Xiaozhi AI-IoT Vietnam Team
 */
#pragma once

#include <lvgl.h>
#include <string>
#include <cstdint>

namespace qrcode {

class QRCodeDisplay {
public:
    QRCodeDisplay() = default;
    ~QRCodeDisplay();

    // Non-copyable
    QRCodeDisplay(const QRCodeDisplay&) = delete;
    QRCodeDisplay& operator=(const QRCodeDisplay&) = delete;

    /**
     * @brief Show a QR code on screen.
     *
     * Creates an LVGL canvas and draws the QR code centred on it.
     * Must be called with the LVGL lock held.
     *
     * @param qrcode       Encoded QR data (from esp_qrcode library).
     * @param screen_width  Display width in pixels.
     * @param screen_height Display height in pixels.
     * @param status_bar_h  Height of status bar (canvas starts below it).
     * @param text          Optional text rendered below the QR code.
     *                      If null, ip_address_ is used.
     */
    void Show(const uint8_t* qrcode,
              int screen_width, int screen_height,
              int status_bar_h,
              const char* text = nullptr);

    /**
     * @brief Remove the QR code canvas.  Must be called with LVGL lock held.
     */
    void Clear();

    /** @return true if QR code is currently displayed. */
    bool IsDisplayed() const { return displayed_; }

    /** Store an IP address string shown under the QR code by default. */
    void SetIpAddress(const std::string& ip) { ip_address_ = ip; }

    /** Get the stored IP address. */
    const std::string& GetIpAddress() const { return ip_address_; }

private:
    lv_obj_t*  canvas_        = nullptr;
    uint16_t*  canvas_buffer_ = nullptr;
    int        canvas_width_  = 0;
    int        canvas_height_ = 0;
    bool       displayed_     = false;
    std::string ip_address_;
};

}  // namespace qrcode
