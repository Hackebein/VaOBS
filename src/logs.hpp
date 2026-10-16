// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vaobs {
namespace fs = std::filesystem;

inline fs::path vrchat_log_directory()
{
    wchar_t home[32768]{};
    const DWORD size = GetEnvironmentVariableW(L"USERPROFILE", home, DWORD(std::size(home)));
    if (!size || size >= std::size(home)) return {};
    return fs::path(home) / L"AppData/LocalLow/VRChat/VRChat";
}

inline std::string media_url(std::string url)
{
    if (url.compare(0, 8, "rtspt://") == 0) url.replace(0, 5, "rtmp");
    return url;
}

inline uint64_t file_time(FILETIME time)
{
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

struct Client {
    fs::path log;
    DWORD pid = 0;
    uint64_t started = 0;
    std::string key() const { return "log:" + log.filename().u8string(); }
    std::string label() const { return log.filename().u8string() + " (PID " + std::to_string(pid) + ")"; }
};

// Match current processes to logs created during startup, in launch order.
// Last-write times never identify a client: ordinary log messages change them.
inline std::vector<Client> running_clients(const fs::path &directory = vrchat_log_directory())
{
    std::vector<Client> processes, clients;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return clients;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return clients;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"VRChat.exe")) continue;
        DWORD process_session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &process_session) || process_session != session) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            processes.push_back({{}, entry.th32ProcessID, file_time(created)});
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    if (processes.empty() || directory.empty()) return clients;
    std::sort(processes.begin(), processes.end(), [](const Client &a, const Client &b) {
        return a.started != b.started ? a.started < b.started : a.pid < b.pid;
    });
    std::vector<std::pair<uint64_t, fs::path>> logs;
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (name.compare(0, 11, L"output_log_") || it->path().extension() != L".txt") continue;
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            logs.emplace_back(file_time(info.ftCreationTime), it->path());
    }
    std::sort(logs.begin(), logs.end());
    std::set<fs::path> assigned;
    for (auto process : processes) {
        for (const auto &log : logs) {
            if (log.first < process.started || assigned.count(log.second)) continue;
            if (log.first > process.started + 1200000000ULL) break;
            process.log = log.second; clients.push_back(process); assigned.insert(log.second); break;
        }
    }
    return clients;
}

struct Playback {
    enum Type { ready, pending, clear } type;
    std::string key, url;
};

inline std::optional<Playback> parse_playback(const std::string &line)
{
    const auto video = line.find("[Video Playback] ");
    if (video != std::string::npos) {
        const auto start = video + 17;
        auto quoted = [&](size_t from) {
            const auto first = line.find('\'', from);
            const auto last = first == std::string::npos ? first : line.find('\'', first + 1);
            return last != std::string::npos ? line.substr(first + 1, last - first - 1) : std::string{};
        };
        const std::string key = quoted(start);
        if (!key.empty()) {
            if (line.compare(start, 15, "Resolving URL '") == 0)
                return Playback{Playback::ready, key, media_url(key)};
            if (line.compare(start, 27, "Attempting to resolve URL '") == 0)
                return Playback{Playback::pending, key, {}};
            if (line.compare(start, 5, "URL '") == 0) {
                const auto resolved = line.find("resolved to '", start + 5);
                const auto url = resolved == std::string::npos ? std::string{} : quoted(resolved);
                if (!url.empty()) return Playback{Playback::ready, key, media_url(url)};
            }
        }
    }
    const auto joining = line.find("[Behaviour] Joining wrld_");
    if (joining != std::string::npos) {
        std::string instance = line.substr(joining + 20);
        if (!instance.empty() && instance.back() == '\r') instance.pop_back();
        return Playback{Playback::clear, std::move(instance), {}};
    }
    if (line.find("VRCApplication: HandleApplicationQuit") != std::string::npos ||
        line.find("[Behaviour] Successfully left room") != std::string::npos) return Playback{Playback::clear, {}, {}};
    return {};
}

inline std::optional<std::string> playback_event(const std::string &line)
{
    const auto event = parse_playback(line);
    if (!event || event->type == Playback::pending) return {};
    return event->url;
}

inline int64_t wall_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline int64_t log_time_ms(const std::string &line)
{
    int year, month, day, hour, minute, second;
    if (std::sscanf(line.c_str(), "%d.%d.%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 1970 || year > 2200 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
    std::tm time{};
    time.tm_year = year - 1900; time.tm_mon = month - 1; time.tm_mday = day;
    time.tm_hour = hour; time.tm_min = minute; time.tm_sec = second; time.tm_isdst = -1;
    const auto value = std::mktime(&time);
    return value < 0 ? 0 : int64_t(value) * 1000;
}

struct PlaybackPosition {
    int64_t time_ms = 0, recorded_at = 0;
    uint64_t revision = 0;
    bool paused = false, stopped = false;
    int64_t at(int64_t now) const { return time_ms + (paused ? 0 : std::max(int64_t(0), now - recorded_at)); }
};

struct Video {
    std::string key, url;
    uint64_t revision = 0;
    PlaybackPosition position{};
    std::string label() const { return key.size() > 120 ? key.substr(0, 117) + "..." : key; }
};

enum class StreamState { checking, online, unavailable };

// The OBS worker verifies decoded audio, not just an HTTP success response.
// A failed refresh gets a second attempt before a working URL is hidden.
class StreamChecks {
    struct Entry {
        StreamState state = StreamState::checking;
        unsigned failures = 0;
        uint64_t due = 0;
        bool busy = false, playback_blocked = false;
    };
    std::map<std::string, Entry> entries;
public:
    void sync(const std::vector<Video> &videos)
    {
        std::set<std::string> present;
        for (const auto &video : videos) if (!video.url.empty()) {
            present.insert(video.url); entries.try_emplace(video.url);
        }
        for (auto it = entries.begin(); it != entries.end();)
            if (!present.count(it->first)) it = entries.erase(it); else ++it;
    }
    StreamState state(const std::string &url) const
    {
        const auto it = entries.find(url);
        return it == entries.end() ? StreamState::checking : it->second.state;
    }
    bool playback_blocked(const std::string &url) const
    {
        const auto it = entries.find(url);
        return it != entries.end() && it->second.playback_blocked;
    }
    void fail_playback(const std::string &url, uint64_t now)
    {
        const auto it = entries.find(url);
        if (it == entries.end()) return;
        auto &entry = it->second;
        entry.playback_blocked = true; entry.state = StreamState::unavailable;
        entry.failures = std::max(2U, entry.failures + 1);
        entry.due = now + std::min(uint64_t(300000), uint64_t(30000) << std::min(entry.failures - 2, 4U));
    }
    uint64_t retry_in(const std::string &url, uint64_t now) const
    {
        const auto it = entries.find(url);
        return it == entries.end() || it->second.due <= now ? 0 : (it->second.due - now + 999) / 1000;
    }
    void retry()
    {
        for (auto &entry : entries) {
            entry.second.due = 0; entry.second.failures = 0;
            entry.second.state = StreamState::checking;
            entry.second.playback_blocked = false;
        }
    }
    std::optional<std::string> take(uint64_t now, const std::string &preferred)
    {
        auto chosen = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->second.busy || it->second.due > now) continue;
            if (chosen == entries.end() || it->second.due < chosen->second.due || it->first == preferred) chosen = it;
            if (it->first == preferred) break;
        }
        if (chosen == entries.end()) return {};
        chosen->second.busy = true;
        return chosen->first;
    }
    void finish(const std::string &url, bool success, uint64_t now)
    {
        const auto it = entries.find(url);
        if (it == entries.end()) return; // A newer resolved URL replaced it.
        auto &entry = it->second;
        entry.busy = false;
        // A probe opened before the player's failure cannot undo its cooldown.
        if (entry.playback_blocked && now < entry.due) return;
        if (success) { entry.state = StreamState::online; entry.failures = 0; entry.playback_blocked = false; }
        else if (++entry.failures >= 2) entry.state = StreamState::unavailable;
        // Avoid repeatedly hitting an expired, blocked, or rate-limited URL.
        const uint64_t backoff = entry.failures == 1 ? 5000 :
            std::min(uint64_t(300000), uint64_t(30000) << std::min(entry.failures - 2, 4U));
        entry.due = now + (success ? 30000 : backoff);
    }
};

inline std::vector<Video> available_videos(const std::vector<Video> &videos, const StreamChecks *checks)
{
    std::vector<Video> result;
    std::set<std::string> urls;
    for (const auto &video : videos)
        if (!video.url.empty() && !video.position.stopped &&
            (!checks || checks->state(video.url) == StreamState::online) && urls.insert(video.url).second)
            result.push_back(video);
    return result;
}

class Videos {
    uint64_t sequence = 0;
    std::map<std::string, std::string> players;
public:
    std::vector<Video> items;
    std::string instance;
    uint64_t epoch = 0;
    void clear(const std::string &world = {}) { items.clear(); players.clear(); instance = world; ++epoch; }
    // VideoTXL records explicit seeks with a player ID. Keep its media clock
    // separate from URL revisions so position updates do not reopen the URL.
    void observe_position(const std::string &line)
    {
        const auto start = line.find("[VideoTXL:");
        const auto end = start == std::string::npos ? start : line.find("] ", start);
        if (end == std::string::npos) return;
        const auto player = line.substr(start + 10, end - start - 10);
        std::string message = line.substr(end + 2);
        if (!message.empty() && message.back() == '\r') message.pop_back();
        if (message.compare(0, 10, "Load Url: ") == 0) { players[player] = message.substr(10); return; }
        const auto key = players.find(player);
        if (key == players.end()) return;
        const auto video = std::find_if(items.begin(), items.end(), [&](const Video &v) { return v.key == key->second; });
        const auto time = log_time_ms(line);
        if (video == items.end() || !time) return;
        auto &position = video->position;
        if (message.compare(0, 10, "Set time: ") == 0) {
            char *tail = nullptr;
            const double seconds = std::strtod(message.c_str() + 10, &tail);
            if (tail == message.c_str() + 10 || *tail || !std::isfinite(seconds) || seconds < 0 || seconds > 1e8) return;
            const auto target = int64_t(std::llround(seconds * 1000));
            if (position.revision && std::abs(position.at(time) - target) <= 1000) return;
            position.time_ms = target; position.stopped = false;
        } else if (message == "Pause" || message == "Play" || message == "Stop") {
            const bool paused = message != "Play";
            const bool stopped = message == "Stop";
            if (position.paused == paused && position.stopped == stopped) return;
            if (position.revision) position.time_ms = position.at(time);
            position.paused = paused; position.stopped = stopped;
            if (!position.revision && !stopped) return;
        } else return;
        position.recorded_at = time; position.revision = ++sequence;
    }
    void observe(const Playback &event)
    {
        if (event.type == Playback::clear) { clear(event.key); return; }
        auto it = std::find_if(items.begin(), items.end(), [&](const Video &v) { return v.key == event.key; });
        if (it == items.end()) {
            // Logs have no universal player ID. Keep a bounded list of URLs
            // detected in this instance, identified by their original URL.
            if (items.size() == 32) items.erase(items.begin());
            items.push_back({event.key, {}, 0, {}}); it = items.end() - 1;
        }
        if (event.type == Playback::ready) {
            it->url = event.url; it->revision = ++sequence;
            it->position = {};
            std::rotate(it, it + 1, items.end());
        }
    }
    Video choose(const std::string &selection, const StreamChecks *checks = nullptr) const
    {
        const Video *chosen = nullptr;
        for (const auto &video : items) {
            if (selection != "latest") {
                if (video.key == selection && (!checks || !checks->playback_blocked(video.url))) return video;
            } else if (!video.url.empty() && (!checks || checks->state(video.url) == StreamState::online) &&
                       !video.position.stopped &&
                       (!chosen || video.revision > chosen->revision)) chosen = &video;
        }
        return chosen ? *chosen : Video{};
    }
};

// Keep a candidate long enough to test its audio. Log recency only decides
// the order of untried candidates; unrelated players cannot steal a match.
class StreamSearch {
    Video current;
    std::map<std::string, std::string> tried;
public:
    void reset() { current = {}; tried.clear(); }
    void select(const Video &video) { reset(); current = video; }
    size_t candidate() const { return tried.size() + 1; }
    Video choose(const Videos &videos, const StreamChecks *checks, bool advance)
    {
        for (auto it = tried.begin(); it != tried.end();)
            if (std::none_of(videos.items.begin(), videos.items.end(), [&](const Video &v) { return v.key == it->first; }))
                it = tried.erase(it);
            else ++it;
        if (advance && !current.key.empty()) tried[current.key] = current.url;
        const Video *next = nullptr, *latest = nullptr;
        for (const auto &video : videos.items) {
            if (video.url.empty() || video.position.stopped || (checks && checks->state(video.url) != StreamState::online)) continue;
            if (!advance && video.key == current.key) { current = video; return current; }
            if (!latest || video.revision > latest->revision) latest = &video;
            const auto attempt = tried.find(video.key);
            if ((attempt == tried.end() || attempt->second != video.url) && (!next || video.revision > next->revision)) next = &video;
        }
        if (!next && latest) { tried.clear(); next = latest; }
        current = next ? *next : Video{};
        return current;
    }
};

class LogCursor {
    fs::path path;
    uint64_t offset = 0, identity = 0;
    DWORD volume = 0;
    bool identified = false, skipping = false, ended = false;
    std::string pending;
    Videos streams;
public:
    explicit LogCursor(fs::path file = {}) : path(std::move(file)) {}
    const fs::path &file() const { return path; }
    bool closed() const { return ended; }
    const Videos &videos() const { return streams; }

    // Snapshot the length and share the file with VRChat, including renames.
    // Incomplete records stay buffered; oversized records are skipped whole.
    std::optional<std::string> read(bool *opened = nullptr)
    {
        if (opened) *opened = false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{file};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info)) return {};
        const uint64_t length = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const uint64_t index = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        if (length < offset || (identified && (identity != index || volume != info.dwVolumeSerialNumber))) {
            offset = 0; pending.clear(); skipping = false; ended = false;
            streams.clear();
        }
        identity = index; volume = info.dwVolumeSerialNumber; identified = true;
        LARGE_INTEGER position{}; position.QuadPart = LONGLONG(offset);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return {};
        if (opened) *opened = true;
        std::optional<std::string> event;
        char bytes[65536];
        while (offset < length) {
            DWORD count = 0;
            if (!ReadFile(file, bytes, DWORD(std::min(uint64_t(sizeof(bytes)), length - offset)), &count, nullptr) || !count) break;
            offset += count;
            for (DWORD i = 0; i < count; ++i) {
                const char ch = bytes[i];
                if (ch == '\n') {
                    if (!skipping) {
                        if (pending.find("VRCApplication: HandleApplicationQuit") != std::string::npos) ended = true;
                        if (auto next = parse_playback(pending)) {
                            streams.observe(*next);
                            if (next->type != Playback::pending) event = next->url;
                        }
                        streams.observe_position(pending);
                    }
                    pending.clear(); skipping = false;
                } else if (!skipping) {
                    pending += ch;
                    if (pending.size() > 1024 * 1024) { pending.clear(); skipping = true; }
                }
            }
        }
        return event;
    }
};

class LogMonitor {
    LogCursor cursor;
    std::string selected, message = "Waiting for a VRChat client log";
    std::optional<std::string> last_url;
    std::set<std::string> closed_clients;
    std::string video_mode = "latest";
    Video selected_video;
    StreamSearch search;

    std::optional<std::string> handover(const std::string &url)
    {
        if (last_url && *last_url == url) return {};
        last_url = url; return url;
    }
public:
    const fs::path &file() const { return cursor.file(); }
    const std::string &status() const { return message; }
    const std::vector<Video> &videos() const { return cursor.videos().items; }
    size_t candidate() const { return search.candidate(); }
    void retry() { search.reset(); selected_video = {}; }

    std::optional<Video> next(const StreamChecks *checks)
    {
        const auto choices = available_videos(videos(), checks);
        if (choices.size() < 2) return {};
        const auto current = std::find_if(choices.begin(), choices.end(), [&](const Video &video) {
            return video.url == selected_video.url;
        });
        const auto index = current == choices.end() ? 0 : (size_t(current - choices.begin()) + 1) % choices.size();
        selected_video = choices[index]; last_url = selected_video.url;
        if (video_mode == "latest") search.select(selected_video);
        else video_mode = selected_video.key;
        return selected_video;
    }

    std::optional<std::string> poll(const std::vector<Client> &clients, const std::string &selection = "first",
                                    const std::string &video_selection = "latest", const StreamChecks *checks = nullptr,
                                    bool advance = false)
    {
        const bool mode_changed = video_mode != video_selection;
        video_mode = video_selection;
        if (mode_changed) { search.reset(); advance = false; }
        const bool automatic = selection == "first" || selection == "newest";
        const Client *choice = nullptr;
        for (const auto &client : clients) {
            if (closed_clients.count(client.key())) continue;
            if (!automatic && client.key() != selection) continue;
            if (!choice || (selection == "newest" ? client.started > choice->started : client.started < choice->started))
                choice = &client;
        }
        if (!choice) {
            cursor = LogCursor{}; selected.clear();
            selected_video = {}; search.reset();
            message = automatic ? "Waiting for a VRChat client log" : "The selected VRChat client is not running";
            return handover("");
        }
        message = "Following " + choice->label();
        if (selected != choice->key() || cursor.file() != choice->log) {
            LogCursor next(choice->log);
            bool opened = false;
            next.read(&opened);
            if (!opened) {
                cursor = LogCursor{}; selected.clear();
                search.reset();
                message = "Cannot read the selected VRChat client log";
                return handover("");
            }
            if (next.closed()) {
                closed_clients.insert(choice->key()); return poll(clients, selection, video_selection, checks, advance);
            }
            const bool new_instance = !cursor.videos().instance.empty() && !next.videos().instance.empty() &&
                cursor.videos().instance != next.videos().instance;
            cursor = std::move(next); selected = choice->key();
            if (new_instance) { search.reset(); advance = false; }
            const auto video = choose_video(video_selection, checks, advance);
            selected_video = video;
            describe_video(video, video_selection);
            if (new_instance) { last_url = video.url; return video.url; }
            return handover(video.url);
        }
        const auto previous = selected_video;
        const auto epoch = cursor.videos().epoch;
        cursor.read();
        if (cursor.closed()) {
            closed_clients.insert(choice->key()); return poll(clients, selection, video_selection, checks, advance);
        }
        if (epoch != cursor.videos().epoch) { search.reset(); advance = false; }
        const auto video = choose_video(video_selection, checks, advance);
        selected_video = video;
        describe_video(video, video_selection);
        if (epoch != cursor.videos().epoch) { last_url = video.url; return video.url; }
        if (mode_changed) return handover(video.url);
        // A fresh record from the selected client can intentionally replay
        // the selected video. Other players must not interrupt a pinned URL.
        if (previous.key != video.key || previous.revision != video.revision) {
            last_url = video.url; return video.url;
        }
        return {};
    }

private:
    Video choose_video(const std::string &selection, const StreamChecks *checks, bool advance)
    {
        return selection == "latest" ? search.choose(cursor.videos(), checks, advance) : cursor.videos().choose(selection, checks);
    }
    void describe_video(const Video &video, const std::string &selection)
    {
        if (!video.url.empty()) message += "\nVideo: " + video.label();
        else if (selection != "latest") message += "\nWaiting for selected video";
    }
};
} // namespace vaobs
