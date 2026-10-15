// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

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

inline std::optional<std::string> playback_event(const std::string &line)
{
    const auto video = line.find("[Video Playback] ");
    if (video != std::string::npos) {
        const auto start = video + 17;
        size_t value = std::string::npos;
        if (line.compare(start, 15, "Resolving URL '") == 0) value = start + 15;
        else if (line.compare(start, 3, "URL") == 0) {
            const auto resolved = line.find("resolved to '", start + 3);
            if (resolved != std::string::npos) value = resolved + 13;
        }
        if (value != std::string::npos) {
            const auto end = line.find('\'', value);
            if (end != std::string::npos && end > value) return media_url(line.substr(value, end - value));
        }
    }
    if (line.find("VRCApplication: HandleApplicationQuit") != std::string::npos ||
        line.find("[Behaviour] Successfully left room") != std::string::npos) return std::string{};
    return {};
}

class LogCursor {
    fs::path path;
    uint64_t offset = 0, identity = 0;
    DWORD volume = 0;
    bool identified = false, skipping = false;
    std::string pending;
public:
    explicit LogCursor(fs::path file = {}) : path(std::move(file)) {}
    const fs::path &file() const { return path; }

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
            offset = 0; pending.clear(); skipping = false;
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
                    if (!skipping) if (auto next = playback_event(pending)) event = std::move(next);
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
    fs::path directory;
    LogCursor cursor;
    HANDLE changes = INVALID_HANDLE_VALUE;
    std::chrono::steady_clock::time_point next_scan{};
public:
    explicit LogMonitor(fs::path dir = vrchat_log_directory()) : directory(std::move(dir)) {}
    ~LogMonitor() { if (changes != INVALID_HANDLE_VALUE) FindCloseChangeNotification(changes); }
    LogMonitor(const LogMonitor &) = delete;
    LogMonitor &operator=(const LogMonitor &) = delete;
    const fs::path &file() const { return cursor.file(); }

    std::optional<std::string> poll()
    {
        if (directory.empty()) return {};
        if (changes == INVALID_HANDLE_VALUE)
            changes = FindFirstChangeNotificationW(directory.c_str(), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
        const bool notified = changes != INVALID_HANDLE_VALUE && WaitForSingleObject(changes, 0) == WAIT_OBJECT_0;
        if (notified && !FindNextChangeNotification(changes)) {
            FindCloseChangeNotification(changes); changes = INVALID_HANDLE_VALUE;
        }
        const auto now = std::chrono::steady_clock::now();
        if (notified || now >= next_scan || cursor.file().empty()) {
            next_scan = now + std::chrono::seconds(5);
            std::error_code ec;
            fs::path latest;
            fs::file_time_type newest = fs::file_time_type::min();
            fs::directory_iterator it(directory, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().wstring();
                if (name.size() < 15 || name.compare(0, 11, L"output_log_") || it->path().extension() != L".txt") continue;
                std::error_code error;
                if (!it->is_regular_file(error) || error) continue;
                const auto modified = it->last_write_time(error);
                if (!error && (modified > newest || (modified == newest && it->path() > latest))) {
                    latest = it->path(); newest = modified;
                }
            }
            if (!latest.empty() && latest != cursor.file()) {
                LogCursor next(latest);
                bool opened = false;
                auto event = next.read(&opened);
                if (opened) { cursor = std::move(next); return event.value_or(""); }
            }
        }
        return cursor.read();
    }
};
} // namespace vaobs
