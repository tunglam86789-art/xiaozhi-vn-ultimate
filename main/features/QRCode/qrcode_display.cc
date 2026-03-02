/**
 * @file qrcode_display.cc
 * @brief QR code display component — draws a QR code on its own LVGL canvas.
 *
 * Contributors: Xiaozhi AI-IoT Vietnam Team
 */

#include "qrcode_display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <qrcode.h>
#include <cstring>

static const char* TAG = "QRCodeDisplay";

namespace qrcode {

QRCodeDisplay::~QRCodeDisplay() {
    // Caller should Clear() with LVGL lock before destruction,
    // but guard against leaks anyway.
    if (canvas_) {
        lv_obj_del(canvas_);
        canvas_ = nullptr;
    }
    if (canvas_buffer_) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Show — create canvas, draw QR
// ---------------------------------------------------------------------------

void QRCodeDisplay::Show(const uint8_t* qrcode,
                         int screen_width, int screen_height,
                         int status_bar_h,
                         const char* text) {
    if (!qrcode) return;

    // Tear down any previous canvas
    Clear();

    canvas_width_  = screen_width;
    canvas_height_ = screen_height - status_bar_h;

    // Allocate RGB565 buffer in SPIRAM
    canvas_buffer_ = static_cast<uint16_t*>(
        heap_caps_malloc(canvas_width_ * canvas_height_ * sizeof(uint16_t),
                         MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (!canvas_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer (%dx%d)", canvas_width_, canvas_height_);
        return;
    }

    // Create LVGL canvas
    canvas_ = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(canvas_, canvas_buffer_, canvas_width_, canvas_height_,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas_, 0, status_bar_h);
    lv_obj_set_size(canvas_, canvas_width_, canvas_height_);
    lv_canvas_fill_bg(canvas_, lv_color_make(0xFF, 0xFF, 0xFF), LV_OPA_COVER);
    lv_obj_move_foreground(canvas_);

    // —— Draw QR modules ——
    int qr_size   = esp_qrcode_get_size(qrcode);
    int max_side   = ((canvas_width_ < canvas_height_) ? canvas_width_ : canvas_height_) * 70 / 100;
    int pixel_size = max_side / qr_size;
    if (pixel_size < 2) pixel_size = 2;

    int qr_pos_x = (canvas_width_  - qr_size * pixel_size) / 2;
    int qr_pos_y = (canvas_height_ - qr_size * pixel_size) / 2;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = lv_color_black();
    rect_dsc.bg_opa   = LV_OPA_COVER;

    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                lv_area_t a = {
                    .x1 = (int32_t)(x * pixel_size + qr_pos_x),
                    .y1 = (int32_t)(y * pixel_size + qr_pos_y),
                    .x2 = (int32_t)((x + 1) * pixel_size - 1 + qr_pos_x),
                    .y2 = (int32_t)((y + 1) * pixel_size - 1 + qr_pos_y),
                };
                lv_draw_rect(&layer, &rect_dsc, &a);
            }
        }
    }

    // —— Draw text below the QR code ——
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_palette_main(LV_PALETTE_ORANGE);
    label_dsc.text  = (text && text[0]) ? text : ip_address_.c_str();

    int th = lv_font_get_line_height(label_dsc.font);
    int text_pos_y = canvas_height_ - qr_pos_y + (qr_pos_y - th) / 2;
    lv_area_t text_area = {
        .x1 = (int32_t)qr_pos_x,
        .y1 = (int32_t)text_pos_y,
        .x2 = (int32_t)(canvas_width_ - 1),
        .y2 = (int32_t)(canvas_height_ - 1),
    };
    lv_draw_label(&layer, &label_dsc, &text_area);

    lv_canvas_finish_layer(canvas_, &layer);
    displayed_ = true;

    ESP_LOGI(TAG, "QR code displayed (%dx%d, pixel=%d)", qr_size, qr_size, pixel_size);
}

// ---------------------------------------------------------------------------
// Clear — remove canvas
// ---------------------------------------------------------------------------

void QRCodeDisplay::Clear() {
    if (canvas_) {
        lv_obj_del(canvas_);
        canvas_ = nullptr;
    }
    if (canvas_buffer_) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
    }
    displayed_ = false;
}

}  // namespace qrcode
