# MP4 Player Feature — MP4 Playback with esp_extractor

## Overview

This component adds MP4 playback support for files stored on the SD card under the `video/` directory.
It uses `espressif/esp_extractor` to demux MP4 containers, then routes:

- H264 video frames to the H.264 software decoder, I420→RGB565 color conversion, and LCD panel
- MP3 audio frames to the audio simple decoder and board speaker

This feature coexists with the existing AVI player and is intentionally narrower:

- Container: MP4 only
- Video codec: H264 only (software decode)
- Audio codec: MP3 only
- Video output: direct LCD draw in RGB565

## Files

| File | Description |
|------|-------------|
| `features/mp4/mp4_player.h` | `Mp4Player` public API, state, playlist, and callbacks |
| `features/mp4/mp4_player.cc` | MP4 extraction, H264 decode, MP3 decode, LCD/audio output |
| `features/mp4/mp4.md` | This document |
| `CMakeLists.txt` | Added `features/mp4/mp4_player.cc` to SOURCES and `features/mp4` to INCLUDE_DIRS |
| `application.h` | Added `Mp4Player*`, `InitMp4Video()`, `PlayMp4Video()`, `kMp4Video` media component |
| `application.cc` | Wired init/stop/play lifecycle for MP4 player alongside AVI player |

## Playback Pipeline

```text
SD card file (.mp4)
    -> esp_extractor (MP4 demux)
        -> audio track (MP3)
            -> esp_audio_simple_dec (frame decode)
            -> AudioCodec::OutputData()
            -> I2S speaker output
        -> video track (H264)
            -> esp_h264_dec_sw (software decode, output I420)
            -> esp_imgfx_color_convert (I420 -> RGB565_BE)
            -> esp_lcd_panel_draw_bitmap()
            -> LCD panel
```

## Key Design Points

1. `esp_extractor` owns all MP4 demuxing. `esp_extractor_register_default()` is called once at init.
2. The player accepts only MP4 files with H264 + MP3 tracks. Other codecs are rejected early.
3. Video is decoded to I420 by `esp_h264_dec_sw` and converted to RGB565 before drawing.
4. Audio is decoded with `esp_audio_simple_dec` (MP3 type, frame mode) and sent to the board's audio codec.
5. H264 NALUs are processed in a loop using the `consume` field to handle multi-NALU packets.
6. Files are discovered from `/<sd-mount>/video/` and sorted alphabetically.
7. Playback runs in a dedicated FreeRTOS task so the application thread is not blocked.
8. The MP4 player is a singleton, initialized alongside the AVI player in `Application::InitMp4Video()`.
9. `StopOtherMedia()` stops MP4 playback when other media starts and vice versa.

## Public API

```cpp
Mp4Player& player = Mp4Player::GetInstance();

player.Initialize(panel, lcd_w, lcd_h, codec, sd_card);
player.ScanDirectory();
player.Play("/sdcard/video/demo.mp4");
player.PlayFile("demo.mp4");
player.Pause();
player.Resume();
player.Stop();
player.Next();
player.Prev();
```

From `Application`:
```cpp
Application::GetInstance().PlayMp4Video("/sdcard/video/demo.mp4");
```

## Directory Layout

Place media files here:

```text
/sdcard/video/*.mp4
```

## Requirements

- MP4 container with H264 video + MP3 audio tracks
- Video resolution must fit the LCD panel (no scaling in v1)
- ESP32-S3 or ESP32-P4 target (esp_extractor and esp_h264 are target-gated)

## Troubleshooting

- If playback fails immediately, check the MP4 track codecs with `esp_extractor_get_stream_info`.
- If video is larger than the LCD, reduce the source resolution or add a scaling step.
- If audio is silent, verify that the board's audio codec is enabled and the MP3 track is present.
- If `InitMp4Video()` logs "display is not LCD", the board uses OLED and cannot render video.
