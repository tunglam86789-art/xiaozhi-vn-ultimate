/**
 * @file mcp_server_features.h
 * @brief MCP tool registrations for media feature modules.
 *
 * Each static method receives a component pointer and registers
 * the appropriate MCP tools, capturing the pointer in lambdas.
 * This avoids calling Application::GetInstance() from tool handlers.
 */
#ifndef MCP_SERVER_FEATURES_H
#define MCP_SERVER_FEATURES_H

class Esp32Music;
class Esp32Radio;
class Esp32SdMusic;
class VideoPlayer;

/**
 * @brief Static helper that registers MCP tools per media component.
 *
 * Call individual methods from Application::InitXxx() after the
 * component is created and initialized.
 */
class McpFeatureTools {
public:
    /** Network / QR code tool (no component dependency). */
    static void RegisterIp2QrCodeTool();

    /** Online music tools (play_song, set_display_mode). */
    static void RegisterMusicTools(Esp32Music* music);

    /** Radio tools (play_station, play_url, stop, get_stations, set_display_mode). */
    static void RegisterRadioTools(Esp32Radio* radio);

    /** SD card music tools (playback, mode, track, directory, search, etc.). */
    static void RegisterSdMusicTools(Esp32SdMusic* sd_music);

    /** SD card video tools (play_video). */
    static void RegisterSdVideoTools(VideoPlayer* video);

private:
    McpFeatureTools() = delete;
};

#endif // MCP_SERVER_FEATURES_H
