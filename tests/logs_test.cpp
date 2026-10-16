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
        require(!playback_event("[Video Playback] Attempting to resolve URL 'https://www.twitch.tv/rossmakr'"),
            "Webpage URLs wait for their resolved media URL");
        require(playback_event("[Behaviour] Joining wrld_other:12345") == "", "Joining another instance clears playback");
        Videos positions;
        positions.observe({Playback::ready, "file-a", "resolved-a"});
        positions.observe({Playback::ready, "file-b", "resolved-b"});
        positions.observe_position("2026.10.15 23:46:43 Debug - [VideoTXL:AVPro-0] Load Url: file-a");
        positions.observe_position("2026.10.15 23:46:44 Debug - [VideoTXL:AVPro-1] Load Url: file-b");
        positions.observe_position("2026.10.15 23:46:52 Debug - [VideoTXL:AVPro-0] Set time: 12.5\r");
        const auto began = log_time_ms("2026.10.15 23:46:52");
        auto clock = positions.choose("file-a").position;
        require(clock.revision && clock.at(began + 600000) == 612500 && !positions.choose("file-b").position.revision,
            "A late OBS start follows the correct player's timestamped media position");
        const auto revision = positions.choose("file-a").revision;
        positions.observe_position("2026.10.15 23:47:02 Debug - [VideoTXL:AVPro-0] Pause");
        clock = positions.choose("file-a").position;
        require(clock.paused && clock.at(began + 600000) == 22500, "Paused media position does not advance with wall time");
        positions.observe_position("2026.10.15 23:47:12 Debug - [VideoTXL:AVPro-0] Play");
        clock = positions.choose("file-a").position;
        require(!clock.paused && clock.at(began + 30000) == 32500 && positions.choose("file-a").revision == revision,
            "Resume advances the media clock without reopening its URL");
        positions.observe_position("2026.10.15 23:47:13 Debug - [VideoTXL:AVPro-0] Play");
        positions.observe_position("2026.10.15 23:47:22 Debug - [VideoTXL:AVPro-0] Set time: 32.5");
        require(positions.choose("file-a").position.revision == clock.revision,
            "Repeated play and routine clock confirmations do not reset alignment");
        positions.observe_position("2026.10.15 23:47:13 Debug - [VideoTXL:AVPro-0] Set time: nan");
        positions.observe_position("[VideoTXL:AVPro-0] Set time: 99");
        require(positions.choose("file-a").position.revision == clock.revision, "Malformed or undated seeks are ignored");
        positions.observe_position("2026.10.15 23:47:23 Debug - [VideoTXL:AVPro-0] Stop");
        require(positions.choose("file-a").position.stopped && positions.choose("latest").key == "file-b",
            "Automatic selection skips a stopped player even if its URL still works");
        positions.observe_position("2026.10.15 23:47:24 Debug - [VideoTXL:AVPro-0] Play");
        require(!positions.choose("file-a").position.stopped, "Resumed players become candidates again");
        positions.clear(); positions.observe({Playback::ready, "file-a", "resolved-a"});
        positions.observe_position("2026.10.15 23:47:14 Debug - [VideoTXL:AVPro-0] Set time: 99");
        require(!positions.choose("file-a").position.revision, "World changes discard player position mappings");
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
        std::vector<Client> clients{{file, 10, 100}};
        LogMonitor monitor;
        require(monitor.poll(clients) == "replacement", "Initial scan");
        append(file, "[Video Playback] Resolving URL 'append'\n");
        require(monitor.poll(clients, "first", "latest", nullptr, true) == "append", "Search advances to the appended candidate");
        const auto next = root / "output_log_2.txt";
        append(next, "New VRChat session\n");
        clients.push_back({next, 20, 200});
        require(!monitor.poll(clients), "First client stays selected when another starts");
        append(next, "[Video Playback] Resolving URL 'append'\n");
        fs::last_write_time(next, fs::file_time_type::clock::now() + std::chrono::seconds(10));
        require(!monitor.poll(clients), "Unrelated writes from another client do not replay old URLs");
        require(!monitor.poll(clients, "newest") && monitor.file() == next, "Same video handover does not restart");
        append(file, "unrelated old-client message\n");
        fs::last_write_time(file, fs::file_time_type::clock::now() + std::chrono::seconds(20));
        require(!monitor.poll(clients, "newest") && monitor.file() == next, "Newest means startup order, not latest write");
        append(next, "[Video Playback] Resolving URL 'append'\n");
        require(monitor.poll(clients, "newest") == "append", "Selected client can intentionally replay same video");
        append(file, "[Video Playback] Resolving URL 'manual'\n");
        require(monitor.poll(clients, clients.front().key(), "latest", nullptr, true) == "manual", "Manual client selection");
        append(next, "[Video Playback] Resolving URL 'other'\n");
        require(!monitor.poll(clients, clients.front().key()), "Manual selection ignores the other client");
        clients.erase(clients.begin());
        require(monitor.poll(clients, "log:output_log_1.txt") == "", "Missing manual client clears instead of switching");
        require(!monitor.poll(clients, "log:output_log_1.txt"), "Missing manual client does not repeatedly clear playback");
        require(monitor.poll(clients, "first") == "other", "Automatic mode selects remaining client");
        append(next, "[Behaviour] Successfully left room\n");
        require(monitor.poll(clients) == "", "Leave clears selected client playback");
        append(next, "[Video Playback] Resolving URL 'continued'\n");
        require(monitor.poll(clients) == "continued", "Leave does not retire selected client");
        append(file, "[Video Playback] Resolving URL 'continued'\n");
        clients.insert(clients.begin(), {file, 10, 100});
        require(!monitor.poll(clients), "Returning first client with same video does not restart");
        append(file, "VRCApplication: HandleApplicationQuit\n");
        require(!monitor.poll(clients) && monitor.file() == next, "Client quit hands over same video without interruption");
        clients.clear();
        require(monitor.poll(clients) == "", "All clients closed clears playback");

        const auto multi_file = root / "output_log_multi.txt";
        append(multi_file,
            "[Behaviour] Joining wrld_old:1\n[Video Playback] Resolving URL 'stale-video'\n"
            "[Behaviour] Joining wrld_current:2\n"
            "[Video Playback] Attempting to resolve URL 'https://www.twitch.tv/rossmakr'\n"
            "[Video Playback] Attempting to resolve URL 'https://www.twitch.tv/unbeatablemusic'\n"
            "[Video Playback] URL 'https://www.twitch.tv/rossmakr' resolved to 'https://cdn.test/ross.m3u8'\n"
            "[Video Playback] URL 'https://www.twitch.tv/unbeatablemusic' resolved to 'https://cdn.test/other.m3u8'\n"
            "[Video Playback] Resolving URL 'player-three'\n[Video Playback] Resolving URL 'player-four'\n");
        const std::vector<Client> multiple{{multi_file, 30, 300}};
        const std::string ross = "https://www.twitch.tv/rossmakr";
        LogMonitor multi;
        require(multi.poll(multiple, "first", ross) == "https://cdn.test/ross.m3u8",
            "Four players in one read retain each original-to-resolved mapping");
        require(multi.videos().size() == 4, "Catalog excludes old instances and duplicate resolved URLs");
        append(multi_file, "[Video Playback] URL 'https://www.twitch.tv/unbeatablemusic' resolved to 'https://cdn.test/other-new.m3u8'\n");
        require(!multi.poll(multiple, "first", ross), "Other player refresh cannot interrupt pinned stream");
        append(multi_file, "[Video Playback] URL 'https://www.twitch.tv/rossmakr' resolved to 'https://cdn.test/ross.m3u8'\n");
        require(multi.poll(multiple, "first", ross) == "https://cdn.test/ross.m3u8", "Pinned stream may intentionally replay");
        append(multi_file, "[Video Playback] URL 'https://www.twitch.tv/rossmakr' resolved to 'https://cdn.test/ross-new.m3u8'\n");
        require(multi.poll(multiple, "first", ross) == "https://cdn.test/ross-new.m3u8", "Pinned stream follows resolved URL refresh");
        require(multi.poll(multiple, "first", "https://www.twitch.tv/unbeatablemusic") == "https://cdn.test/other-new.m3u8",
            "Changing video selection uses its latest resolved URL");
        require(multi.poll(multiple, "first", ross) == "https://cdn.test/ross-new.m3u8", "Return to pinned stream");
        append(multi_file, "[Behaviour] Joining wrld_new:3\n[Video Playback] Resolving URL 'other-world-video'\n");
        require(multi.poll(multiple, "first", ross) == "" && multi.videos().size() == 1,
            "Instance switch discards prior streams and waits for manual selection");
        require(!multi.poll(multiple, "first", ross), "Missing pinned URL never follows another player");
        require(multi.poll(multiple) == "other-world-video", "Latest mode still supports ordinary single-player worlds");

        const auto health_file = root / "output_log_health.txt";
        append(health_file, "[Video Playback] Resolving URL 'online'\n[Video Playback] Resolving URL 'offline'\n");
        const std::vector<Client> health_clients{{health_file, 40, 400}};
        StreamChecks checks;
        LogMonitor health;
        require(health.poll(health_clients, "first", "latest", &checks) == "", "Unverified URLs are not auto-selected");
        checks.sync(health.videos());
        require(checks.take(0, "online") == "online", "Check the preferred URL first");
        checks.finish("online", true, 0);
        require(health.poll(health_clients, "first", "latest", &checks) == "online", "Health updates select an online URL without new log events");
        checks.finish("offline", false, 0);
        require(checks.state("offline") == StreamState::checking, "One failed check must not declare a URL offline");
        checks.finish("offline", false, 5000);
        require(checks.state("offline") == StreamState::unavailable && !health.poll(health_clients, "first", "latest", &checks),
            "Latest unavailable URL does not interrupt a working stream");
        checks.finish("online", false, 30000);
        require(checks.state("online") == StreamState::online, "A transient refresh failure keeps a known-good stream");
        checks.finish("online", false, 35000);
        require(health.poll(health_clients, "first", "latest", &checks) == "", "All unavailable URLs clear automatic playback");
        require(checks.take(64999, "online") == "offline", "Unavailable streams are retried in the background");
        checks.finish("offline", true, 65000);
        require(health.poll(health_clients, "first", "latest", &checks) == "offline", "A recovered stream becomes selectable again");
        checks.finish("offline", false, 95000); checks.finish("offline", false, 100000);
        require(!health.poll(health_clients, "first", "offline", &checks), "An unavailable manual selection remains pinned instead of switching");
        append(health_file, "[Video Playback] URL 'offline' resolved to 'refreshed-url'\n");
        require(health.poll(health_clients, "first", "offline", &checks) == "refreshed-url", "Pinned selection follows refreshed URLs even before checking");
        checks.sync(health.videos());
        require(checks.state("refreshed-url") == StreamState::checking, "A refreshed URL never inherits an old offline result");
        checks.finish("offline", true, 100001);
        require(checks.state("offline") == StreamState::checking, "Ignore late results for replaced URLs");
        StreamChecks retries;
        retries.sync({{"retry", "retry-url", 1}});
        retries.finish("retry-url", false, 0);
        require(retries.retry_in("retry-url", 0) == 5, "A transient failure gets an early retry");
        retries.finish("retry-url", false, 5000);
        require(retries.retry_in("retry-url", 5000) == 30, "An unavailable stream backs off");
        retries.finish("retry-url", false, 35000);
        require(retries.retry_in("retry-url", 35000) == 60, "Repeated failures increase retry spacing");
        for (int i = 0; i < 20; ++i) retries.finish("retry-url", false, 100000);
        require(retries.retry_in("retry-url", 100000) == 300, "Retry backoff is bounded at five minutes");
        retries.retry();
        require(retries.state("retry-url") == StreamState::checking && retries.take(100001, "retry-url") == "retry-url",
            "Manual retry immediately checks failed streams again");
        retries.finish("retry-url", true, 100001);
        require(retries.state("retry-url") == StreamState::online && retries.retry_in("retry-url", 100001) == 30,
            "Recovery clears failure backoff");
        retries.fail_playback("retry-url", 110000);
        require(retries.playback_blocked("retry-url") && retries.retry_in("retry-url", 110000) == 30,
            "A stalled player gets a retry cooldown");
        retries.finish("retry-url", true, 110001);
        require(retries.state("retry-url") == StreamState::unavailable && !retries.take(139999, "retry-url"),
            "A late successful probe cannot keep a stalled player selected");
        require(retries.take(140000, "retry-url") == "retry-url", "The failed connection is retried after cooldown");
        retries.finish("retry-url", true, 140001);
        require(!retries.playback_blocked("retry-url") && retries.state("retry-url") == StreamState::online,
            "A fresh successful check allows the player to reconnect");
        retries.fail_playback("retry-url", 150000); retries.retry();
        require(!retries.playback_blocked("retry-url"), "Manual retry clears a player failure immediately");
        const auto search_file = root / "output_log_search.txt";
        append(search_file, "[Behaviour] Joining wrld_search:1\n"
            "[Video Playback] Resolving URL 'a'\n[Video Playback] Resolving URL 'b'\n"
            "[Video Playback] Resolving URL 'c'\n[Video Playback] Resolving URL 'd'\n");
        const std::vector<Client> search_clients{{search_file, 50, 500}};
        StreamChecks search_checks;
        LogMonitor search;
        search.poll(search_clients, "first", "latest", &search_checks);
        search_checks.sync(search.videos());
        for (const auto &video : search.videos()) search_checks.finish(video.url, true, 0);
        require(search.poll(search_clients, "first", "latest", &search_checks) == "d", "Start with newest confirmed candidate");
        append(search_file, "[Video Playback] Resolving URL 'a'\n");
        require(!search.poll(search_clients, "first", "latest", &search_checks), "Other players cannot steal the candidate under test or a locked match");
        for (const char *expected : {"a", "c", "b", "a"})
            require(search.poll(search_clients, "first", "latest", &search_checks, true) == expected,
                "Exhaust candidates once per round before retrying, even if log order changes");
        search_checks.finish("a", false, 30000); search_checks.finish("a", false, 35000);
        require(search.poll(search_clients, "first", "latest", &search_checks) == "d", "Unavailable candidate is skipped without waiting for audio mismatch");
        require(search.poll(search_clients, "first", "b", &search_checks, true) == "b", "Manual URL ignores search feedback");
        require(!search.poll(search_clients, "first", "b", &search_checks, true), "Manual choice remains pinned through repeated matching failures");
        require(search.poll(search_clients, "first", "latest", &search_checks) == "d", "Changing back to automatic starts a fresh search");
        require(!search.poll(search_clients, "first", "d", &search_checks), "Pinning the same URL keeps its live connection");
        require(!search.poll(search_clients, "first", "latest", &search_checks), "Unpinning the same URL keeps its live connection");
        append(search_file, "[Behaviour] Joining wrld_search:2\n[Video Playback] Resolving URL 'a'\n");
        require(search.poll(search_clients, "first", "latest", &search_checks, true) == "", "New instance clears old search candidates");
        search_checks.finish("a", true, 40000);
        require(search.poll(search_clients, "first", "latest", &search_checks) == "a", "New instance retries previously rejected streams");
        require(!search.poll(search_clients, "first", "latest", &search_checks, true), "A sole candidate is retried without restarting its live connection");
        require(!search.next(&search_checks), "Next leaves a sole URL playing without restarting it");
        const auto next_file = root / "output_log_next.txt";
        append(next_file, "[Video Playback] Resolving URL 'one'\n[Video Playback] Resolving URL 'two'\n"
            "[Video Playback] URL 'two-alias' resolved to 'two'\n[Video Playback] Resolving URL 'offline'\n"
            "[Video Playback] Resolving URL 'stopped'\n"
            "2026.10.15 23:46:43 Debug - [VideoTXL:AVPro-0] Load Url: stopped\n"
            "2026.10.15 23:46:44 Debug - [VideoTXL:AVPro-0] Stop\n");
        const std::vector<Client> next_clients{{next_file, 60, 600}};
        LogMonitor manual_next; StreamChecks next_checks;
        manual_next.poll(next_clients, "first", "latest", &next_checks);
        require(!manual_next.next(&next_checks), "Next ignores URLs that are still checking");
        next_checks.sync(manual_next.videos());
        for (const auto *url : {"one", "two", "stopped"}) next_checks.finish(url, true, 0);
        next_checks.finish("offline", false, 0); next_checks.finish("offline", false, 5000);
        require(available_videos(manual_next.videos(), &next_checks).size() == 2,
            "Next skips offline, stopped, and duplicate resolved URLs");
        require(manual_next.poll(next_clients, "first", "latest", &next_checks) == "two", "Automatic starts on the latest available URL");
        for (const auto *expected : {"one", "two", "one"}) {
            const auto chosen = manual_next.next(&next_checks);
            require(chosen && chosen->url == expected, "Next wraps and always selects a different available URL");
            require(!manual_next.poll(next_clients, "first", "latest", &next_checks), "Automatic search keeps the manually advanced candidate");
        }
        require(manual_next.poll(next_clients, "first", "latest", &next_checks, true) == "two",
            "Automatic matching still advances after Next");
        manual_next.poll(next_clients, "first", "one", &next_checks);
        const auto pinned_next = manual_next.next(&next_checks);
        require(pinned_next && pinned_next->url == "two" &&
            !manual_next.poll(next_clients, "first", pinned_next->key, &next_checks, true), "Next can change a manual URL and keeps it pinned");
        next_checks.finish("one", false, 30000); next_checks.finish("one", false, 35000);
        require(!manual_next.next(&next_checks), "Next becomes a no-op if an alternative goes offline");
        std::cout << "PASS: clients, URL refresh, instances, offline filtering, recovery, automatic stream cycling, Next and pinned selection\n";
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        std::error_code ignored; fs::remove_all(root, ignored); return 1;
    }
}
