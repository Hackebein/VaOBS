// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLogCursorKeepsPartialLinesAndHandlesTruncation(t *testing.T) {
	path := filepath.Join(t.TempDir(), "output_log_test.txt")
	line := "[Video Playback] Resolving URL 'https://example.invalid/video'\n"
	split := len(line) / 2
	if err := os.WriteFile(path, []byte(line[:split]), 0600); err != nil {
		t.Fatal(err)
	}
	cursor := logCursor{path: path}
	if _, changed, err := cursor.read(); err != nil || changed {
		t.Fatalf("partial record processed: %v", err)
	}
	appendText(t, path, line[split:])
	if url, changed, err := cursor.read(); err != nil || !changed || url != "https://example.invalid/video" {
		t.Fatalf("split URL lost: %q %v %v", url, changed, err)
	}
	if _, changed, err := cursor.read(); err != nil || changed {
		t.Fatal("unchanged log was processed again")
	}
	if err := os.WriteFile(path, []byte("VRCApplication: HandleApplicationQuit\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if url, changed, err := cursor.read(); err != nil || !changed || url != "" {
		t.Fatal("truncated log did not clear playback")
	}
}

func appendText(t *testing.T, path, text string) {
	t.Helper()
	file, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	_, writeErr := file.WriteString(text)
	closeErr := file.Close()
	if writeErr != nil || closeErr != nil {
		t.Fatalf("append: %v %v", writeErr, closeErr)
	}
}

func TestLongLogLineDoesNotBlockFollowingURLs(t *testing.T) {
	path := filepath.Join(t.TempDir(), "output_log_test.txt")
	text := strings.Repeat("x", 1024*1024+1) + "\n[Video Playback] Resolving URL 'https://example.invalid/new'\n"
	if err := os.WriteFile(path, []byte(text), 0600); err != nil {
		t.Fatal(err)
	}
	cursor := logCursor{path: path}
	if url, changed, err := cursor.read(); err != nil || !changed || url != "https://example.invalid/new" {
		t.Fatalf("long line blocked reading: %q %v", url, err)
	}
}

func TestLogWatcherSwitchesFilesAndStops(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "output_log_first.txt")
	if err := os.WriteFile(path, []byte("[Video Playback] Resolving URL 'first'\n"), 0600); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	updates := make(chan string, 8)
	done := make(chan struct{})
	go func() { defer close(done); watchLogs(ctx, dir, func(url string) { updates <- url }) }()
	waitURL := func(want string) {
		t.Helper()
		select {
		case got := <-updates:
			if got != want {
				t.Fatalf("got %q, want %q", got, want)
			}
		case <-time.After(7 * time.Second):
			t.Fatalf("no update for %q", want)
		}
	}
	waitURL("first")
	newPath := filepath.Join(dir, "output_log_second.txt")
	if err := os.WriteFile(newPath, []byte("[Video Playback] Resolving URL 'second'\n"), 0600); err != nil {
		t.Fatal(err)
	}
	now := time.Now().Add(time.Second)
	if err := os.Chtimes(newPath, now, now); err != nil {
		t.Fatal(err)
	}
	waitURL("second")
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("log watcher did not stop")
	}
}
