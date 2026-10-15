// SPDX-License-Identifier: GPL-2.0-or-later
#include "logs.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
void append(const vaobs::fs::path &file, const std::string &text)
{
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out << text;
    if (!out) throw std::runtime_error("Could not write test log");
}
int main()
{
    using namespace vaobs;
    const auto root = fs::temp_directory_path() / ("vaobs-log-test-" + std::to_string(GetCurrentProcessId()));
    try {
        require(!playback_event("unrelated 'https://example.test'"), "Unrelated lines");
        require(playback_event("[Video Playback] Resolving URL 'https://example.test/a'") == "https://example.test/a", "Resolving URL");
        require(playback_event("[Video Playback] URL 'x' resolved to 'https://example.test/b'") == "https://example.test/b", "Resolved URL");
        require(playback_event("[Video Playback] Resolving URL 'rtspt://example.test/live'") == "rtmp://example.test/live", "RTSP-over-TCP rewrite");
        require(playback_event("VRCApplication: HandleApplicationQuit") == "", "Quit clears playback");
        fs::create_directories(root);
        const auto file = root / "output_log_1.txt";
        append(file, "[Video Playback] Resolving URL 'https://example.test/");
        LogCursor cursor(file);
        require(!cursor.read(), "Partial line must wait");
        append(file, "one'\r\n");
        require(cursor.read() == "https://example.test/one", "Complete split line");
        require(!cursor.read(), "Unchanged log must not replay");
        append(file, "[Video Playback] Resolving URL 'https://example.test/one'\n");
        require(cursor.read() == "https://example.test/one", "Repeated URL is a new event");
        append(file, std::string(1024 * 1024 + 1, 'x') + "[Video Playback] Resolving URL 'bad'\n[Behaviour] Successfully left room\n");
        require(cursor.read() == "", "Skip oversized line then process leave");
        std::ofstream(file, std::ios::trunc) << "[Video Playback] Resolving URL 'new'\n";
        require(cursor.read() == "new", "Truncation resets cursor");
        fs::rename(file, root / "old.txt");
        append(file, std::string(200, ' ') + "[Video Playback] Resolving URL 'replacement'\n");
        require(cursor.read() == "replacement", "Same-path replacement resets cursor");
        LogMonitor monitor(root);
        require(monitor.poll() == "replacement", "Initial scan");
        append(file, "[Video Playback] Resolving URL 'append'\n");
        require(monitor.poll() == "append", "500-ms polling reads append");
        const auto next = root / "output_log_2.txt";
        append(next, "New VRChat session\n");
        fs::last_write_time(next, fs::file_time_type::clock::now() + std::chrono::seconds(2));
        require(monitor.poll() == "", "New log clears previous URL");
        append(next, "[Video Playback] Resolving URL 'latest'\n");
        require(monitor.poll() == "latest", "New log playback");
        std::cout << "PASS: URL parsing, split/oversized records, repeat, truncate, replacement, and log switching\n";
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        std::error_code ignored; fs::remove_all(root, ignored); return 1;
    }
}
