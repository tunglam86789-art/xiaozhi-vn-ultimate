#ifndef ESP32_SD_MUSIC_H
#define ESP32_SD_MUSIC_H

/**
 * @file esp32_sd_music.h
 * @brief SD card music player -- inherits AudioStreamPlayer.
 *
 * Responsibilities:
 *   - Playlist management (playlist.json, scanning, genres, suggestions)
 *   - Track metadata (ID3v1 / ID3v2)
 *   - File-based audio streaming via AudioStreamPlayer base class
 *   - Pause / resume / shuffle / repeat
 *   - WAV / MP3 / AAC / FLAC playback from SD card
 */

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "audio_stream_player.h"

class Esp32SdMusic : public AudioStreamPlayer {
public:
    // ============================================================
    // 1) ENUM -- State Machine
    // ============================================================
    enum class PlayerState {
        Stopped = 0,
        Preparing,
        Playing,
        Paused,
        Error
    };

    // ============================================================
    // 2) ENUM -- Repeat modes
    // ============================================================
    enum class RepeatMode {
        None = 0,
        RepeatOne,
        RepeatAll
    };

    // ============================================================
    // 3) Struct TrackInfo
    // ============================================================
    struct TrackInfo {
        std::string name;
        std::string path;

        std::string title;
        std::string artist;
        std::string album;
        std::string genre;
        std::string comment;
        std::string year;
        int         track_number = 0;

        int    duration_ms  = 0;
        int    bitrate_kbps = 0;
        size_t file_size    = 0;

        uint32_t    cover_size = 0;
        std::string cover_mime;
    };

    // ============================================================
    // 4) Struct Progress
    // ============================================================
    struct TrackProgress {
        int64_t position_ms = 0;
        int64_t duration_ms = 0;
    };

public:
    // ============================================================
    // 5) Constructor / Destructor
    // ============================================================
    Esp32SdMusic();
    ~Esp32SdMusic();

    void Initialize(class SdCard* sd_card);

    // ============================================================
    // 6) Playlist API
    // ============================================================
    bool loadTrackList();
    size_t getTotalTracks() const;
    std::vector<TrackInfo> listTracks() const;

    bool setDirectory(const std::string& relative_dir);
    bool playDirectory(const std::string& relative_dir);

    bool playByName(const std::string& keyword);
    TrackInfo getTrackInfo(int index) const;
    bool setTrack(int index);

    std::string getCurrentTrack() const;
    std::string getCurrentTrackPath() const;

    std::vector<std::string> listDirectories() const;
    std::vector<TrackInfo> searchTracks(const std::string& keyword) const;

    std::string resolveLongName(const std::string& path);
    std::string resolveCaseInsensitiveDir(const std::string& path);

    size_t countTracksInDirectory(const std::string& relative_dir);
    size_t countTracksInCurrentDirectory() const;

    std::vector<TrackInfo> listTracksPage(size_t page_index,
                                          size_t page_size = 10) const;
    bool rebuildPlaylistFromSd();

    // ============================================================
    // 7) Playback API
    // ============================================================
    bool play();
    void pause();
    void stop();

    bool next();
    bool prev();
    bool IsPlaying() const override;

    // ============================================================
    // 8) Playback Settings
    // ============================================================
    void shuffle(bool enabled);
    void repeat(RepeatMode mode);

    // ============================================================
    // 9) Query state / FFT
    // ============================================================
    PlayerState getState() const;
    TrackProgress updateProgress() const;

    int64_t getDurationMs() const;
    int64_t getCurrentPositionMs() const;
    int getBitrate() const;
    std::string getDurationString() const;
    std::string getCurrentTimeString() const;

    // ============================================================
    // 10) Suggestions
    // ============================================================
    std::vector<TrackInfo> suggestNextTracks(size_t max_results = 5);
    std::vector<TrackInfo> suggestSimilarTo(const std::string& name_or_path,
                                            size_t max_results = 5);

    // ============================================================
    // 11) Genre Playlists
    // ============================================================
    bool buildGenrePlaylist(const std::string& genre);
    bool playGenreIndex(int pos);
    bool playNextGenre();
    std::vector<std::string> listGenres() const;

protected:
    // ============================================================
    // AudioStreamPlayer overrides
    // ============================================================
    void SourceDataLoop(const std::string& source) override;
    void OnStreamInfoReady(int sample_rate, int bits_per_sample,
                           int channels) override;
    void OnPcmFrame(int64_t play_time_ms, int sample_rate,
                    int channels) override;
    void OnPlaybackFinished() override;
    void OnDisplayReady() override;
    void OnPauseStateChanged(bool paused) override;

private:
    // ============================================================
    // Playlist helpers
    // ============================================================
    void scanDirectoryRecursive(const std::string& dir,
                                std::vector<TrackInfo>& out);
    int findNextTrackIndex(int start, int direction);
    bool resolveDirectoryRelative(const std::string& relative_dir,
                                  std::string& out_full);
    int findTrackIndexByKeyword(const std::string& keyword) const;

    bool loadPlaylistFromFile(const std::string& playlist_path,
                              std::vector<TrackInfo>& out) const;
    bool savePlaylistToFile(const std::string& playlist_path,
                            const std::vector<TrackInfo>& list) const;

    // ============================================================
    // Audio format detection
    // ============================================================
    AudioDecoderType DetectFileFormat(const std::string& path) const;

    // ID3 / WAV helpers
    size_t SkipId3Tag(uint8_t* data, size_t size);

    // ============================================================
    // History / suggestions
    // ============================================================
    void recordPlayHistory(int index);
    void handleNextTrack();

private:
    SdCard* sd_card_;

    // Playlist
    std::string root_directory_;
    std::vector<TrackInfo> playlist_;
    mutable std::mutex playlist_mutex_;
    int current_index_ = -1;
    std::vector<uint32_t> play_count_;

    // State
    std::atomic<PlayerState> state_;

    // Playback options
    bool shuffle_enabled_;
    RepeatMode repeat_mode_;

    // Progress tracking
    std::atomic<int64_t> total_duration_ms_;

    // Genre playlists
    std::vector<int> genre_playlist_;
    int genre_current_pos_ = -1;
    std::string genre_current_key_;

    // History / suggestions
    mutable std::mutex history_mutex_;
    std::vector<int> play_history_indices_;
};

#endif // ESP32_SD_MUSIC_H
