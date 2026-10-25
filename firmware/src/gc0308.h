// StackChan 公式の Arduino カメラ例 (docs.m5stack.com/en/arduino/stackchan/camera) と同じ GC0308 ラッパー。
// 画角は VGA (640x480) で初期化する。esp32-camera はフレームバッファを初期化時のサイズで確保するため、
// 後から大きい解像度へ set_framesize しない。
#pragma once

#include <M5Unified.h>
#include "esp_camera.h"

class GC0308 {
 public:
  camera_fb_t* fb = nullptr;
  sensor_t* sensor = nullptr;

  bool begin() {
    camera_config_t config = {};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = -1;
    config.pin_sscb_sda = 12;
    config.pin_sscb_scl = 11;
    config.pin_d0 = 39;
    config.pin_d1 = 40;
    config.pin_d2 = 41;
    config.pin_d3 = 42;
    config.pin_d4 = 15;
    config.pin_d5 = 16;
    config.pin_d6 = 48;
    config.pin_d7 = 47;
    config.pin_vsync = 46;
    config.pin_href = 38;
    config.pin_pclk = 45;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 0;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.sccb_i2c_port = -1;

    M5.In_I2C.release();  // カメラの SCCB が内部 I2C バスを使うため解放する
    if (esp_camera_init(&config) != ESP_OK) return false;
    sensor = esp_camera_sensor_get();
    return true;
  }

  bool get() {
    fb = esp_camera_fb_get();
    return fb != nullptr;
  }

  bool free() {
    if (!fb) return false;
    esp_camera_fb_return(fb);
    fb = nullptr;
    return true;
  }
};
