# Video Player Feature — AVI Playback on ESP32-S3

## Overview

This component provides AVI video playback from SD card on ESP32-S3 boards with LCD displays.
It integrates the `espressif/avi_player` library with the existing project audio/display infrastructure.

---

## Files

| File | Description |
|------|-------------|
| `features/video/video_player.h` | `VideoPlayer` class — full public API, structs, constants |
| `features/video/video_player.cc` | Implementation — AVI demux, JPEG decode, LCD draw, PCM audio output |
| `features/video/video.md` | This document |

### Modified Files

| File | Change |
|------|--------|
| `display/lcd_display.h` | Added `GetPanelHandle()` public accessor for direct LCD rendering |
| `CMakeLists.txt` | Added `features/video/video_player.cc` to SOURCES and `features/video` to INCLUDE_DIRS |

---

## Architecture

```
SD Card (.avi)
    │
    ▼
espressif/avi_player  (AVI demuxer — FreeRTOS task, core 1)
    │
    ├── video_cb  →  MJPEG compressed frame data
    │       │
    │       ▼
    │   espressif/esp_new_jpeg  (software JPEG → RGB565_LE)
    │       │
    │       ▼
    │   Double-buffered RGB565 PSRAM  (2 × up to 320×240×2 bytes)
    │       │
    │       ▼
    │   esp_lcd_panel_draw_bitmap()  ──►  LCD (direct, bypasses LVGL)
    │
    └── audio_cb  →  raw PCM int16_t
            │
            ▼
        Volume scale  +  channel conversion (mono↔stereo)
            │
            ▼
        AudioCodec::OutputData()  ──►  I2S  ──►  Speaker
```

---

## Dependencies (already in `idf_component.yml`)

| Component | Purpose |
|-----------|---------|
| `espressif/avi_player ^2.0.0` | AVI demuxer + FPS timer |
| `espressif/esp_new_jpeg ^0.6.1` | Software JPEG decoder (MJPEG → RGB565) |
| `espressif/esp_lcd_panel_ops` | `esp_lcd_panel_draw_bitmap()` for direct LCD write |
| `espressif/esp_audio_codec` | Audio codec abstraction (I2S output) |

---

## Key Design Decisions

### 1. Direct LCD Rendering (no LVGL)
Video frames are drawn with `esp_lcd_panel_draw_bitmap()` instead of going through LVGL.
This eliminates LVGL mutex overhead and vsync wait, enabling the highest possible frame rate.

### 2. Double-Buffered PSRAM Frames
Two RGB565 frame buffers (each up to `VIDEO_MAX_WIDTH × VIDEO_MAX_HEIGHT × 2` bytes) are
allocated in PSRAM. While one buffer is drawn to the LCD, the JPEG decoder writes the next
frame into the other — completely tear-free, zero copy.

### 3. JPEG Decode via `esp_new_jpeg`
The `esp_new_jpeg` software decoder is already a project dependency and outputs `RGB565_LE`
natively — the exact pixel format expected by the SPI/RGB LCD panels used in this project.

### 4. Automatic Sample Rate Switching
`AviAudioClockCallback` is invoked once per file with the AVI audio stream parameters.
It calls `AudioCodec::SetOutputSampleRate()` to reconfigure the I2S clock without restarting
the codec. On `Stop()` / `HandlePlayEnd()`, the original sample rate is restored with `-1`.

### 5. Channel Conversion (mono ↔ stereo)
`OutputAudioPcm()` detects mismatches between the AVI audio channel count and the codec's
`output_channels()`:
- Mono AVI + stereo codec → duplicate each sample to L+R.
- Stereo AVI + mono codec → average L+R samples.
- Same count → direct pass-through.

### 6. Singleton Pattern
Consistent with `Application::GetInstance()` and other project singletons.
Prevents duplicate resource allocation across the codebase.

### 7. Virtual Hooks for Extension
Three protected virtual methods allow subclasses to customize behavior without modifying
the core player:

```cpp
virtual void OnVideoFrameReady(uint16_t* rgb565, uint16_t w, uint16_t h);
virtual void OnAudioFrameReady(int16_t* pcm, size_t samples, int channels);
virtual void OnPlaybackStateChanged(VideoPlayerState old, VideoPlayerState new);
```

---

## Configuration Constants (`video_player.h`)

| Constant | Default | Description |
|----------|---------|-------------|
| `VIDEO_AVI_BUFFER_SIZE` | 60 KB | AVI internal read buffer |
| `VIDEO_AVI_TASK_STACK` | 8 KB | AVI player FreeRTOS task stack |
| `VIDEO_AVI_TASK_PRIORITY` | 6 | Task priority |
| `VIDEO_AVI_TASK_CORE` | 1 (APP_CPU) | Pinned core for AVI task |
| `VIDEO_MAX_WIDTH` | 320 | Max video width (frame buffer sizing) |
| `VIDEO_MAX_HEIGHT` | 240 | Max video height (frame buffer sizing) |
| `VIDEO_DEFAULT_DIRECTORY` | `"videos"` | Sub-folder on SD card |
| `VIDEO_MAX_FILES` | 256 | Max files per directory scan |

---

## State Machine

```
  Idle ──Play()──► Loading ──success──► Playing
   ▲                  │                   │
   │                  │ error             ├──Pause()──► Paused
   │                  ▼                   │              │
   │               Error                  │           Resume()
   │                                      │              │
   └───────Stop()────────────────────────-┘◄─────────────┘
                                          │
                                      PlayEnd
                                          │
                                        Idle
```

---

## Usage Example

```cpp
#include "video_player.h"
#include "lcd_display.h"
#include "board.h"

// After Board, Display and SdCard are initialized:

auto* lcd = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
auto& vp  = VideoPlayer::GetInstance();

// Initialize
vp.Initialize(
    lcd->GetPanelHandle(),          // Direct LCD panel handle
    lcd->width(), lcd->height(),     // Display resolution
    Board::GetInstance().GetAudioCodec(),
    Board::GetInstance().GetSdCard()
);

// Register callbacks (optional)
vp.SetEndCallback([](const std::string& path) {
    ESP_LOGI("App", "Finished: %s", path.c_str());
});

// Scan SD card for AVI files in /sdcard/videos/
size_t count = vp.ScanDirectory();
ESP_LOGI("App", "Found %zu videos", count);

// Play a specific file
vp.Play("/sdcard/videos/demo.avi");

// Or play by name from current directory
vp.PlayFile("demo.avi");

// Playlist navigation
vp.Next();
vp.Prev();

// Control
vp.Pause();
vp.Resume();
vp.Stop();

// Set volume (1.0 = 100%)
vp.SetVolume(0.8f);

// Query stats
auto stats = vp.GetStats();
ESP_LOGI("App", "FPS approx: %.1f  Decode avg: %.1f ms",
         1000.0f / stats.avg_decode_ms, stats.avg_decode_ms);
```

---

## Recommended AVI File Format

For best performance on ESP32-S3 with ILI9341 / ST7789 (320×240):

| Parameter | Recommended |
|-----------|-------------|
| Container | AVI (RIFF) |
| Video codec | MJPEG |
| Resolution | 320×240 or smaller |
| Frame rate | 10–20 fps |
| Audio codec | PCM (uncompressed) |
| Audio sample rate | 16000 or 44100 Hz |
| Audio channels | 1 (mono) or 2 (stereo) |
| Audio bits | 16-bit |

**Conversion command (FFmpeg):**
```bash
ffmpeg -i input.mp4 \
  -vf "scale=320:240" \
  -r 15 \
  -vcodec mjpeg -q:v 5 \
  -acodec pcm_s16le -ar 16000 -ac 1 \
  output.avi

ffmpeg -i sample-5s.mp4 -vf "scale=320:240" -r 15 -vcodec mjpeg -q:v 5 -acodec pcm_s16le -ar 16000 -ac 1 sample-5s.avi
```

> For larger displays (e.g. 480×320), increase `VIDEO_MAX_WIDTH` / `VIDEO_MAX_HEIGHT`
> constants in `video_player.h` accordingly. Frame buffer PSRAM usage = `W × H × 2 × 2` bytes.

---

## Memory Usage

| Resource | Location | Size |
|----------|----------|------|
| Frame buffer × 2 | PSRAM | 2 × 320×240×2 = 307 200 bytes ≈ 300 KB |
| AVI read buffer | PSRAM (task heap) | 60 KB |
| JPEG I/O structs | PSRAM | ~64 bytes |
| AVI player task stack | PSRAM | 8 KB |
| **Total PSRAM** | | **≈ 368 KB** |
| AudioCodec output vector | Internal RAM (temporary) | ~4 KB per frame |
