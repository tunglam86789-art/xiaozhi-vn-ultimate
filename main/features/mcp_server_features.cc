/**
 * @file mcp_server_features.cc
 * @brief MCP tool registrations for media features (music, radio, SD, video).
 *
 * Each method receives a component pointer and captures it in tool
 * lambdas, avoiding Application::GetInstance() calls from handlers.
 */

#include "mcp_server_features.h"
#include "mcp_server.h"
#include "board.h"
#include "esp32_music.h"
#include "esp32_radio.h"
#include "esp32_sd_music.h"
#include "features/video/video_player.h"
#include "wifi_station.h"
#include "system_info.h"
#include "display.h"

#include <esp_log.h>
#include <cJSON.h>
#include <qrcode.h>
#include <algorithm>
#include <cstring>

#define TAG "McpFeatures"

/* ================================================================== */
/*  Network / QR code tool                                             */
/* ================================================================== */

void McpFeatureTools::RegisterIp2QrCodeTool() {
    auto& mcp = McpServer::GetInstance();
    auto& board = Board::GetInstance();

    mcp.AddTool("self.network.ip2qrcode",
        "Print the QR code of the IP address connected to WiFi network.\n"
        "Use this tool when user asks about network connection, IP address and print QR code.\n"
        "Returns the new IP address, SSID, and connection status. Also displays IP address as QR code on LCD screen.",
        PropertyList(),
        [&board](const PropertyList& /*properties*/) -> ReturnValue {
            auto& wifi_station = WifiStation::GetInstance();
            ESP_LOGI(TAG, "Getting network status for IP address tool");
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "connected", wifi_station.IsConnected());

            if (wifi_station.IsConnected()) {
                std::string ip_address = wifi_station.GetIpAddress();
                cJSON_AddStringToObject(json, "ip_address", ip_address.c_str());
                cJSON_AddStringToObject(json, "ssid", wifi_station.GetSsid().c_str());
                cJSON_AddNumberToObject(json, "rssi", wifi_station.GetRssi());
                cJSON_AddNumberToObject(json, "channel", wifi_station.GetChannel());
                cJSON_AddStringToObject(json, "mac_address", SystemInfo::GetMacAddress().c_str());
                cJSON_AddStringToObject(json, "status", "connected");

                auto display = board.GetDisplay();
                if (display) {
                    ESP_LOGI(TAG, "Generating QR code for IP address: %s", ip_address.c_str());
                    if (display->QRCodeIsSupported()) {
                        ip_address += "/ota";
                        display->SetIpAddress(ip_address);
                        static Display* s_display = display;
                        esp_qrcode_config_t qrcode_cfg = {
                            .display_func = [](esp_qrcode_handle_t qrcode) {
                                if (s_display && qrcode) {
                                    s_display->DisplayQRCode(qrcode, nullptr);
                                }
                            },
                            .max_qrcode_version = 10,
                            .qrcode_ecc_level = ESP_QRCODE_ECC_MED
                        };
                        std::string qr_text = "http://" + ip_address;
                        esp_err_t err = esp_qrcode_generate(&qrcode_cfg, qr_text.c_str());
                        if (err == ESP_OK) {
                            ESP_LOGI(TAG, "QR code generated for IP: %s", ip_address.c_str());
                            cJSON_AddBoolToObject(json, "qrcode_displayed", true);
                        } else {
                            ESP_LOGE(TAG, "Failed to generate QR code");
                            cJSON_AddBoolToObject(json, "qrcode_displayed", false);
                        }
                    } else {
                        display->SetChatMessage("assistant", ip_address.c_str());
                        vTaskDelay(pdMS_TO_TICKS(5000));
                        cJSON_AddBoolToObject(json, "qrcode_displayed", false);
                    }
                }
            } else {
                cJSON_AddStringToObject(json, "status", "disconnected");
                cJSON_AddStringToObject(json, "message", "Device is not connected to WiFi");
            }
            return json;
        });
}

/* ------------------------------------------------------------------ */
/*  Online Music tools                                                 */
/* ------------------------------------------------------------------ */

void McpFeatureTools::RegisterMusicTools(Esp32Music* music) {
    if (!music) return;

    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.music.play_song",
        "Play a specified song ONLINE. Đây là chế độ PHÁT NHẠC MẶC ĐỊNH.\n"
        "Khi người dùng nói: 'phát nhạc', 'mở nhạc', 'phát bài hát', "
        "'play music', 'play song', 'mở bài ...', AI phải ưu tiên dùng tool này.\n"
        "\n"
        "Chỉ dùng SD card nếu người dùng nói rõ: 'nhạc trong thẻ nhớ', "
        "'nhạc offline', 'bài trong thẻ', 'SD card', 'chạy nhạc nội bộ', v.v.\n"
        "\n"
        "Args:\n"
        "  song_name: Tên bài hát (bắt buộc)\n"
        "  artist_name: Tên ca sĩ (tùy chọn)\n"
        "Return:\n"
        "  Phát bài hát online ngay lập tức.\n",
        PropertyList({
            Property("song_name", kPropertyTypeString),
            Property("artist_name", kPropertyTypeString, "")
        }),
        [music](const PropertyList& properties) -> ReturnValue {
            auto song   = properties["song_name"].value<std::string>();
            auto artist = properties["artist_name"].value<std::string>();
            if (!music->Download(song, artist)) {
                return "{\"success\": false, \"message\": \"Failed to get music resource\"}";
            }
            return "{\"success\": true, \"message\": \"Music started playing\"}";
        });

    mcp.AddTool("self.music.set_display_mode",
        "Set the display mode for music playback. You can choose to display spectrum or lyrics.\n"
        "Args:\n"
        "  `mode`: Display mode, options are 'spectrum' or 'lyrics'.\n"
        "Return:\n"
        "  Setting result information.",
        PropertyList({
            Property("mode", kPropertyTypeString)
        }),
        [music](const PropertyList& properties) -> ReturnValue {
            auto mode_str = properties["mode"].value<std::string>();
            std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);

            auto* esp32_music = static_cast<Esp32Music*>(music);
            if (mode_str == "spectrum") {
                esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_SPECTRUM);
                return "{\"success\": true, \"message\": \"Switched to spectrum display mode\"}";
            } else if (mode_str == "lyrics") {
                esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_LYRICS);
                return "{\"success\": true, \"message\": \"Switched to lyrics display mode\"}";
            }
            return "{\"success\": false, \"message\": \"Invalid display mode, use 'spectrum' or 'lyrics'\"}";
        });
}

/* ------------------------------------------------------------------ */
/*  Radio tools                                                        */
/* ------------------------------------------------------------------ */

void McpFeatureTools::RegisterRadioTools(Esp32Radio* radio) {
    if (!radio) return;

    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.radio.play_station",
        "Play a radio station by name. Use this tool when user requests to play radio or listen to a specific station."
        "VOV mộc/mốc/mốt/mậu/máu/một/mút/mót/mục means VOV1 channel.\n"
        "Args:\n"
        "  `station_name`: The name of the radio station to play (e.g., 'VOV1', 'BBC', 'NPR').\n"
        "Return:\n"
        "  Playback status information.",
        PropertyList({
            Property("station_name", kPropertyTypeString)
        }),
        [radio](const PropertyList& properties) -> ReturnValue {
            auto name = properties["station_name"].value<std::string>();
            if (!radio->PlayStation(name)) {
                return "{\"success\": false, \"message\": \"Failed to find or play radio station: " + name + "\"}";
            }
            return "{\"success\": true, \"message\": \"Radio station " + name + " started playing\"}";
        });

    mcp.AddTool("self.radio.get_stations",
        "Get the list of available radio stations.\n"
        "Return:\n"
        "  JSON array of available radio stations.",
        PropertyList(),
        [radio](const PropertyList& /*properties*/) -> ReturnValue {
            auto stations = radio->GetStationList();
            std::string result = "{\"success\": true, \"stations\": [";
            for (size_t i = 0; i < stations.size(); ++i) {
                result += "\"" + stations[i] + "\"";
                if (i < stations.size() - 1) result += ", ";
            }
            result += "]}";
            return result;
        });
}

/* ------------------------------------------------------------------ */
/*  SD-card Music tools                                                */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_SD_CARD_ENABLE
void McpFeatureTools::RegisterSdVideoTools(VideoPlayer* video) {
    if (!video) return;

    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.sdvideo.play_video",
        "Play a video file from SD.\n"
        "Args:\n"
        "  `video_name`: The name or keyword of the video file to play.\n"
        "  `action`: The action to perform (shuffle | repeat | only).\n"
        "For only: `video_name` (string)\n"
        "For shuffle: `enabled` (bool)\n"
        "For repeat: `mode` = none | one | all\n"
        "Return:\n"
        "  Playback status information.",
        PropertyList({
            Property("video_name", kPropertyTypeString),
            Property("action",  kPropertyTypeString)
        }),
        [video](const PropertyList& properties) -> ReturnValue {
            auto video_name = properties["video_name"].value<std::string>();
            std::string action = properties["action"].value<std::string>();

            if (video->GetPlaylist().empty()) {
                return "{\"success\": false, \"message\": \"No video files found on SD card\"}";
            }

            if (action == "shuffle" || action == "repeat") {
                if (action == "shuffle") {
                    // Pick a random video from the playlist
                    auto& playlist = video->GetPlaylist();
                    int random_idx = rand() % playlist.size();
                    video->Play(playlist[random_idx].path);
                    return "{\"success\": true, \"message\": \"Shuffle: playing random video\"}";
                } else if (action == "repeat") {
                    // Auto-play next video when one ends
                    video->SetEndCallback([video](const std::string& /*finished_path*/) {
                        if (!video->GetPlaylist().empty()) {
                            video->Next(); // Loop through all videos in directory
                        }
                    });
                }
                video->Next();
                return "{\"success\": true, \"message\": \"Shuffle mode enabled, playing next video\"}";
            }

            // Search playlist for matching video name and play it
            video->SetEndCallback(nullptr); // Clear any existing callback
            for (const auto& entry : video->GetPlaylist()) {
                if (entry.path.find(video_name) != std::string::npos) {
                    ESP_LOGI(TAG, "[SdVideo] Playing: %s", entry.path.c_str());
                    video->Play(entry.path);
                    return "{\"success\": true, \"message\": \"Video started playing: " + entry.path + "\"}";
                }
            }
            return "{\"success\": false, \"message\": \"No video matching: " + video_name + "\"}";
        });
}

void McpFeatureTools::RegisterSdMusicTools(Esp32SdMusic* sd_music) {
    if (!sd_music) return;

    auto& mcp = McpServer::GetInstance();

    /* ---------- 1) Playback control ---------- */
    mcp.AddTool("self.sdmusic.playback",
        "Điều khiển phát nhạc từ THẺ NHỚ (SD card).\n"
        "KHÔNG dùng tool này khi người dùng chỉ nói: 'phát nhạc', 'mở bài', "
        "'play music', 'phát bài hát'.\n"
        "\n"
        "Tool này chỉ dùng khi người dùng nói rõ:\n"
        "- nhạc trong thẻ nhớ / nhạc offline / phát bài trong thẻ / SD card\n"
        "\n"
        "action = play | pause | stop | next | prev\n"
        "Return: trạng thái điều khiển SD card.\n",
        PropertyList({
            Property("action", kPropertyTypeString),
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            std::string action = props["action"].value<std::string>();

            if (action == "play") {
                if (sd_music->getTotalTracks() == 0) {
                    if (!sd_music->loadTrackList()) {
                        return "{\"success\": false, \"message\": \"No MP3 files found on SD card\"}";
                    }
                }
                bool ok = sd_music->play();
                return ok ? "{\"success\": true, \"message\": \"Playback started\"}"
                          : "{\"success\": false, \"message\": \"Failed to play\"}";
            }
            if (action == "pause") { sd_music->pause(); return true; }
            if (action == "stop")  { sd_music->stop();  return true; }
            if (action == "next")  { return sd_music->next(); }
            if (action == "prev")  { return sd_music->prev(); }
            return "{\"success\":false,\"message\":\"Unknown playback action\"}";
        });

    /* ---------- 2) Shuffle / Repeat mode ---------- */
    mcp.AddTool("self.sdmusic.mode",
        "Control playback mode: shuffle and repeat.\n"
        "action = shuffle | repeat\n"
        "For shuffle: `enabled` (bool)\n"
        "For repeat: `mode` = none | one | all",
        PropertyList({
            Property("action",  kPropertyTypeString),
            Property("enabled", kPropertyTypeBoolean),
            Property("mode",    kPropertyTypeString)
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            std::string action = props["action"].value<std::string>();

            if (action == "shuffle") {
                bool enabled = props["enabled"].value<bool>();
                sd_music->shuffle(enabled);
                if (enabled) {
                    if (sd_music->getTotalTracks() == 0) sd_music->loadTrackList();
                    if (sd_music->getTotalTracks() == 0) return false;
                    int idx = rand() % sd_music->getTotalTracks();
                    sd_music->setTrack(idx);
                }
                return true;
            }
            if (action == "repeat") {
                std::string mode = props["mode"].value<std::string>();
                if (mode == "none")      sd_music->repeat(Esp32SdMusic::RepeatMode::None);
                else if (mode == "one")  sd_music->repeat(Esp32SdMusic::RepeatMode::RepeatOne);
                else if (mode == "all")  sd_music->repeat(Esp32SdMusic::RepeatMode::RepeatAll);
                else return "Invalid repeat mode";
                return true;
            }
            return "Unknown mode action";
        });

    /* ---------- 3) Track operations ---------- */
    mcp.AddTool("self.sdmusic.track",
        "Track-level operations.\n"
        "action = set | info | list | current\n"
        "  set:     needs `index`\n"
        "  info:    needs `index`\n"
        "  list:    return JSON { count }\n"
        "  current: return name string",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("index",  kPropertyTypeInteger, 0, 0, 9999)
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            std::string action = props["action"].value<std::string>();

            auto ensure_playlist = [sd_music]() {
                if (sd_music->getTotalTracks() == 0) sd_music->loadTrackList();
            };

            if (action == "set") {
                ensure_playlist();
                return sd_music->setTrack(props["index"].value<int>());
            }
            if (action == "info") {
                ensure_playlist();
                auto info = sd_music->getTrackInfo(props["index"].value<int>());
                cJSON* json = cJSON_CreateObject();
                cJSON_AddStringToObject(json, "name",    info.name.c_str());
                cJSON_AddStringToObject(json, "path",    info.path.c_str());
                cJSON_AddStringToObject(json, "title",   info.title.c_str());
                cJSON_AddStringToObject(json, "artist",  info.artist.c_str());
                cJSON_AddStringToObject(json, "album",   info.album.c_str());
                cJSON_AddStringToObject(json, "genre",   info.genre.c_str());
                cJSON_AddStringToObject(json, "comment", info.comment.c_str());
                cJSON_AddStringToObject(json, "year",    info.year.c_str());
                cJSON_AddNumberToObject(json, "track_number", info.track_number);
                cJSON_AddNumberToObject(json, "duration_ms",  info.duration_ms);
                cJSON_AddNumberToObject(json, "bitrate_kbps", info.bitrate_kbps);
                cJSON_AddNumberToObject(json, "file_size",    (double)info.file_size);
                cJSON_AddBoolToObject(json,   "has_cover",    info.cover_size > 0);
                cJSON_AddNumberToObject(json, "cover_size",   (int)info.cover_size);
                cJSON_AddStringToObject(json, "cover_mime",   info.cover_mime.c_str());
                return json;
            }
            if (action == "list") {
                ensure_playlist();
                cJSON* o = cJSON_CreateObject();
                cJSON_AddNumberToObject(o, "count", (int)sd_music->getTotalTracks());
                return o;
            }
            if (action == "current") {
                ensure_playlist();
                return sd_music->getCurrentTrack();
            }
            return "Unknown track action";
        });

    /* ---------- 4) Directory operations ---------- */
    mcp.AddTool("self.sdmusic.directory",
        "Directory-level operations.\n"
        "action = play | list\n"
        "  play: requires `directory`\n"
        "  list: list directories under current root",
        PropertyList({
            Property("action",    kPropertyTypeString),
            Property("directory", kPropertyTypeString)
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            std::string action = props["action"].value<std::string>();

            if (action == "play") {
                std::string dir = props["directory"].value<std::string>();
                if (!sd_music->playDirectory(dir)) {
                    return "{\"success\": false, \"message\": \"Cannot play directory or has no MP3\"}";
                }
                return "{\"success\": true, \"message\": \"Playing directory\"}";
            }
            if (action == "list") {
                cJSON* arr = cJSON_CreateArray();
                auto list = sd_music->listDirectories();
                for (auto& d : list) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(d.c_str()));
                }
                return arr;
            }
            return "{\"success\": false, \"message\": \"Unknown directory action\"}";
        });

    /* ---------- 5) Search / Play by name ---------- */
    mcp.AddTool("self.sdmusic.search",
        "Search and play tracks by name.\n"
        "action = search | play\n"
        "  search: returns matching tracks (needs `keyword`)\n"
        "  play:   play by name (needs `keyword`)",
        PropertyList({
            Property("action",  kPropertyTypeString),
            Property("keyword", kPropertyTypeString)
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            std::string action  = props["action"].value<std::string>();
            std::string keyword = props["keyword"].value<std::string>();

            auto ensure_playlist = [sd_music]() {
                if (sd_music->getTotalTracks() == 0) sd_music->loadTrackList();
            };

            if (action == "search") {
                cJSON* arr = cJSON_CreateArray();
                ensure_playlist();
                auto list = sd_music->searchTracks(keyword);
                for (auto& t : list) {
                    cJSON* o = cJSON_CreateObject();
                    cJSON_AddStringToObject(o, "name",    t.name.c_str());
                    cJSON_AddStringToObject(o, "path",    t.path.c_str());
                    cJSON_AddStringToObject(o, "title",   t.title.c_str());
                    cJSON_AddStringToObject(o, "artist",  t.artist.c_str());
                    cJSON_AddStringToObject(o, "album",   t.album.c_str());
                    cJSON_AddStringToObject(o, "genre",   t.genre.c_str());
                    cJSON_AddStringToObject(o, "year",    t.year.c_str());
                    cJSON_AddNumberToObject(o, "track_number", t.track_number);
                    cJSON_AddNumberToObject(o, "duration_ms",  t.duration_ms);
                    cJSON_AddNumberToObject(o, "bitrate_kbps", t.bitrate_kbps);
                    cJSON_AddBoolToObject(o,   "has_cover",    t.cover_size > 0);
                    cJSON_AddNumberToObject(o, "cover_size",   (int)t.cover_size);
                    cJSON_AddStringToObject(o, "cover_mime",   t.cover_mime.c_str());
                    cJSON_AddItemToArray(arr, o);
                }
                return arr;
            }
            if (action == "play") {
                if (keyword.empty())
                    return "{\"success\": false, \"message\": \"Keyword cannot be empty\"}";
                ensure_playlist();
                bool ok = sd_music->playByName(keyword);
                return ok ? "{\"success\": true, \"message\": \"Playing song by name\"}"
                          : "{\"success\": false, \"message\": \"Song not found\"}";
            }
            return "{\"success\": false, \"message\": \"Unknown search action\"}";
        });

    /* ---------- 6) Library / pagination ---------- */
    mcp.AddTool("self.sdmusic.library",
        "Thông tin THƯ VIỆN BÀI HÁT (tracks).\n"
        "action = count_dir | count_current | page\n"
        "  count_dir: đếm SỐ BÀI HÁT trong thư mục chỉ định\n"
        "  count_current: đếm SỐ BÀI HÁT trong thư mục hiện tại\n"
        "  page: phân trang DANH SÁCH BÀI HÁT\n",
        PropertyList({
            Property("action",    kPropertyTypeString),
            Property("directory", kPropertyTypeString),
            Property("page",      kPropertyTypeInteger, 1, 1, 10000),
            Property("page_size", kPropertyTypeInteger, 10, 1, 1000)
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            std::string action = props["action"].value<std::string>();

            auto ensure_playlist = [sd_music]() {
                if (sd_music->getTotalTracks() == 0) sd_music->loadTrackList();
            };

            if (action == "count_dir") {
                std::string dir = props["directory"].value<std::string>();
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "directory", dir.c_str());
                cJSON_AddNumberToObject(o, "count", (int)sd_music->countTracksInDirectory(dir));
                return o;
            }
            if (action == "count_current") {
                ensure_playlist();
                cJSON* o = cJSON_CreateObject();
                cJSON_AddNumberToObject(o, "count", (int)sd_music->countTracksInCurrentDirectory());
                return o;
            }
            if (action == "page") {
                ensure_playlist();
                int page      = props["page"].value<int>();
                int page_size = props["page_size"].value<int>();
                if (page <= 0) page = 1;
                if (page_size <= 0) page_size = 10;

                size_t page_index = (size_t)(page - 1);
                auto list = sd_music->listTracksPage(page_index, (size_t)page_size);
                size_t start_index = page_index * (size_t)page_size;

                cJSON* arr = cJSON_CreateArray();
                for (size_t i = 0; i < list.size(); ++i) {
                    const auto& t = list[i];
                    cJSON* o = cJSON_CreateObject();
                    cJSON_AddNumberToObject(o, "index",        (int)(start_index + i));
                    cJSON_AddStringToObject(o, "name",         t.name.c_str());
                    cJSON_AddStringToObject(o, "path",         t.path.c_str());
                    cJSON_AddStringToObject(o, "title",        t.title.c_str());
                    cJSON_AddStringToObject(o, "artist",       t.artist.c_str());
                    cJSON_AddStringToObject(o, "album",        t.album.c_str());
                    cJSON_AddStringToObject(o, "genre",        t.genre.c_str());
                    cJSON_AddStringToObject(o, "year",         t.year.c_str());
                    cJSON_AddNumberToObject(o, "track_number", t.track_number);
                    cJSON_AddNumberToObject(o, "duration_ms",  t.duration_ms);
                    cJSON_AddNumberToObject(o, "bitrate_kbps", t.bitrate_kbps);
                    cJSON_AddBoolToObject(o,   "has_cover",    t.cover_size > 0);
                    cJSON_AddNumberToObject(o, "cover_size",   (int)t.cover_size);
                    cJSON_AddStringToObject(o, "cover_mime",   t.cover_mime.c_str());
                    cJSON_AddItemToArray(arr, o);
                }
                return arr;
            }
            return "{\"success\": false, \"message\": \"Unknown library action\"}";
        });

    /* ---------- 7) Reload playlist ---------- */
    mcp.AddTool("self.sdmusic.reload",
        "Quét lại toàn bộ thư viện nhạc trong thẻ SD và cập nhật playlist.json.\n"
        "Return: JSON báo thành công / thất bại.",
        PropertyList(),
        [sd_music](const PropertyList&) -> ReturnValue {
            if (!sd_music->rebuildPlaylistFromSd()) {
                return "{\"success\": false, \"message\": \"Failed to rescan SD card\"}";
            }
            return "{\"success\": true, \"message\": \"SD playlist reloaded\"}";
        });

    /* ---------- 8) Song suggestions ---------- */
    mcp.AddTool("self.sdmusic.suggest",
        "Song suggestion based on history / similarity.\n"
        "action = next | similar\n"
        "  next:    uses `max_results`\n"
        "  similar: uses `keyword` + `max_results`",
        PropertyList({
            Property("action",      kPropertyTypeString),
            Property("keyword",     kPropertyTypeString),
            Property("max_results", kPropertyTypeInteger, 5, 1, 50)
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            cJSON* arr = cJSON_CreateArray();
            std::string action = props["action"].value<std::string>();

            auto ensure_playlist = [sd_music]() {
                if (sd_music->getTotalTracks() == 0) sd_music->loadTrackList();
            };
            ensure_playlist();

            std::string keyword = props["keyword"].value<std::string>();
            int max_results     = props["max_results"].value<int>();
            if (max_results <= 0) max_results = 5;

            auto add_track = [arr](const Esp32SdMusic::TrackInfo& t) {
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "name",    t.name.c_str());
                cJSON_AddStringToObject(o, "path",    t.path.c_str());
                cJSON_AddStringToObject(o, "title",   t.title.c_str());
                cJSON_AddStringToObject(o, "artist",  t.artist.c_str());
                cJSON_AddStringToObject(o, "album",   t.album.c_str());
                cJSON_AddStringToObject(o, "genre",   t.genre.c_str());
                cJSON_AddStringToObject(o, "year",    t.year.c_str());
                cJSON_AddNumberToObject(o, "track_number", t.track_number);
                cJSON_AddNumberToObject(o, "duration_ms",  t.duration_ms);
                cJSON_AddNumberToObject(o, "bitrate_kbps", t.bitrate_kbps);
                cJSON_AddBoolToObject(o,   "has_cover",    t.cover_size > 0);
                cJSON_AddNumberToObject(o, "cover_size",   (int)t.cover_size);
                cJSON_AddStringToObject(o, "cover_mime",   t.cover_mime.c_str());
                cJSON_AddItemToArray(arr, o);
            };

            if (action == "next") {
                for (auto& t : sd_music->suggestNextTracks((size_t)max_results)) add_track(t);
            } else if (action == "similar") {
                for (auto& t : sd_music->suggestSimilarTo(keyword, (size_t)max_results)) add_track(t);
            }
            return arr;
        });

    /* ---------- 9) Progress ---------- */
    mcp.AddTool("self.sdmusic.progress",
        "Get current playback progress and duration.",
        PropertyList(),
        [sd_music](const PropertyList&) -> ReturnValue {
            cJSON* o = cJSON_CreateObject();
            auto prog  = sd_music->updateProgress();
            auto state = sd_music->getState();
            int  br    = sd_music->getBitrate();

            const char* s = "unknown";
            switch (state) {
                case Esp32SdMusic::PlayerState::Stopped:   s = "stopped";   break;
                case Esp32SdMusic::PlayerState::Preparing: s = "preparing"; break;
                case Esp32SdMusic::PlayerState::Playing:   s = "playing";   break;
                case Esp32SdMusic::PlayerState::Paused:    s = "paused";    break;
                case Esp32SdMusic::PlayerState::Error:     s = "error";     break;
            }

            cJSON_AddNumberToObject(o, "position_ms", (int)prog.position_ms);
            cJSON_AddNumberToObject(o, "duration_ms", (int)prog.duration_ms);
            cJSON_AddStringToObject(o, "state",        s);
            cJSON_AddNumberToObject(o, "bitrate_kbps", br);
            cJSON_AddStringToObject(o, "position_str", sd_music->getCurrentTimeString().c_str());
            cJSON_AddStringToObject(o, "duration_str", sd_music->getDurationString().c_str());
            cJSON_AddStringToObject(o, "track_name",   sd_music->getCurrentTrack().c_str());
            cJSON_AddStringToObject(o, "track_path",   sd_music->getCurrentTrackPath().c_str());
            return o;
        });

    /* ---------- 10) Genre operations ---------- */
    mcp.AddTool("self.sdmusic.genre",
        "Genre-based music operations.\n"
        "action = search | play | play_index | next",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("genre",  kPropertyTypeString),
            Property("index",  kPropertyTypeInteger, 0, 0, 9999)
        }),
        [sd_music](const PropertyList& props) -> ReturnValue {
            std::string action = props["action"].value<std::string>();
            std::string genre  = props["genre"].value<std::string>();

            auto ensure_playlist = [sd_music]() {
                if (sd_music->getTotalTracks() == 0) sd_music->loadTrackList();
            };
            ensure_playlist();

            auto ascii_lower = [](std::string s) {
                for (char& c : s) {
                    unsigned char u = (unsigned char)c;
                    if (u < 128) c = std::tolower(u);
                }
                return s;
            };

            if (action == "search") {
                cJSON* arr = cJSON_CreateArray();
                if (genre.empty()) return arr;
                auto all  = sd_music->listTracks();
                auto low  = ascii_lower(genre);
                for (auto& t : all) {
                    if (ascii_lower(t.genre).find(low) != std::string::npos) {
                        cJSON* o = cJSON_CreateObject();
                        cJSON_AddStringToObject(o, "name",   t.name.c_str());
                        cJSON_AddStringToObject(o, "path",   t.path.c_str());
                        cJSON_AddStringToObject(o, "artist", t.artist.c_str());
                        cJSON_AddStringToObject(o, "album",  t.album.c_str());
                        cJSON_AddStringToObject(o, "genre",  t.genre.c_str());
                        cJSON_AddNumberToObject(o, "duration_ms", t.duration_ms);
                        cJSON_AddItemToArray(arr, o);
                    }
                }
                return arr;
            }
            if (action == "play") {
                if (genre.empty())
                    return "{\"success\": false, \"message\": \"Genre cannot be empty\"}";
                if (!sd_music->buildGenrePlaylist(genre))
                    return "{\"success\": false, \"message\": \"No tracks found for this genre\"}";
                bool ok = sd_music->playGenreIndex(0);
                return ok ? "{\"success\": true, \"message\": \"Playing first track of genre\"}"
                          : "{\"success\": false, \"message\": \"Failed to play genre\"}";
            }
            if (action == "play_index") {
                bool ok = sd_music->playGenreIndex(props["index"].value<int>());
                return ok ? "{\"success\": true, \"message\": \"Playing track in genre list\"}"
                          : "{\"success\": false, \"message\": \"Index invalid or genre list empty\"}";
            }
            if (action == "next") {
                bool ok = sd_music->playNextGenre();
                return ok ? "{\"success\": true, \"message\": \"Playing next track in genre\"}"
                          : "{\"success\": false, \"message\": \"No next track or no active genre mode\"}";
            }
            return "{\"success\": false, \"message\": \"Unknown genre action\"}";
        });

    /* ---------- 11) List all genres ---------- */
    mcp.AddTool("self.sdmusic.genre_list",
        "List all unique genres available in the current SD music library.",
        PropertyList(),
        [sd_music](const PropertyList&) -> ReturnValue {
            cJSON* arr = cJSON_CreateArray();
            if (sd_music->getTotalTracks() == 0) sd_music->loadTrackList();
            for (auto& g : sd_music->listGenres()) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(g.c_str()));
            }
            return arr;
        });
}
#endif // CONFIG_SD_CARD_ENABLE