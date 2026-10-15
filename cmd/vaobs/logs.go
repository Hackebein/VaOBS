// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"time"

	"github.com/fsnotify/fsnotify"
)

var (
	urlRegex  = regexp.MustCompile(`\[Video Playback\] (?:URL.*resolved to '|Resolving URL ')([^']+)'`)
	quitRegex = regexp.MustCompile(`VRCApplication: HandleApplicationQuit|\[Behaviour\] Successfully left room`)
)

func getVRChatLogDir() (string, error) {
	home, err := os.UserHomeDir()
	return filepath.Join(home, "AppData", "LocalLow", "VRChat", "VRChat"), err
}

func latestLogFile(dir string) (string, error) {
	files, err := filepath.Glob(filepath.Join(dir, "output_log_*.txt"))
	if err != nil {
		return "", err
	}
	var latest string
	var newest time.Time
	for _, path := range files {
		info, err := os.Stat(path)
		if err == nil && !info.IsDir() && info.ModTime().After(newest) {
			latest, newest = path, info.ModTime()
		}
	}
	if latest == "" {
		return "", fmt.Errorf("waiting for a VRChat log")
	}
	return latest, nil
}

// Keep a partial line across polls; only complete log records change playback.
type logCursor struct {
	path         string
	offset       int64
	pending      []byte
	skipLongLine bool
}

func (c *logCursor) read() (url string, changed bool, err error) {
	file, err := os.Open(c.path)
	if err != nil {
		return "", false, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return "", false, err
	}
	if info.Size() < c.offset {
		c.offset, c.pending, c.skipLongLine = 0, nil, false
	}
	if _, err := file.Seek(c.offset, io.SeekStart); err != nil {
		return "", false, err
	}
	reader := io.LimitReader(file, info.Size()-c.offset)
	buffer := make([]byte, 64*1024)
	for {
		n, readErr := reader.Read(buffer)
		c.offset += int64(n)
		for _, ch := range buffer[:n] {
			if ch == '\n' {
				if !c.skipLongLine {
					if match := urlRegex.FindSubmatch(c.pending); len(match) > 1 {
						url, changed = string(match[1]), true
					} else if quitRegex.Match(c.pending) {
						url, changed = "", true
					}
				}
				c.pending, c.skipLongLine = c.pending[:0], false
			} else if !c.skipLongLine {
				c.pending = append(c.pending, ch)
				if len(c.pending) > 1024*1024 {
					c.pending, c.skipLongLine = nil, true
				}
			}
		}
		if readErr == io.EOF {
			return url, changed, nil
		}
		if readErr != nil {
			return url, changed, readErr
		}
	}
}

// PR #1's 500 ms file polling and directory notifications share one owner.
// A periodic rescan also covers lost notifications and starting before VRChat.
func watchLogs(ctx context.Context, dir string, emit func(string)) {
	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		log.Printf("Log notifications unavailable: %v", err)
	}
	var events <-chan fsnotify.Event
	var errors <-chan error
	watching := false
	if watcher != nil {
		defer watcher.Close()
		events, errors = watcher.Events, watcher.Errors
	}
	poll := time.NewTicker(500 * time.Millisecond)
	defer poll.Stop()
	rescan := time.NewTicker(5 * time.Second)
	defer rescan.Stop()
	var cursor logCursor
	selectLog := func() {
		if watcher != nil && !watching {
			watching = watcher.Add(dir) == nil
		}
		path, err := latestLogFile(dir)
		if err != nil || path == cursor.path {
			return
		}
		cursor = logCursor{path: path}
		url, _, err := cursor.read()
		if err == nil {
			emit(url)
		}
		log.Printf("Monitoring %s", filepath.Base(path))
	}
	selectLog()
	for {
		select {
		case <-ctx.Done():
			return
		case <-poll.C:
			if cursor.path == "" {
				continue
			}
			url, changed, err := cursor.read()
			if err == nil && changed {
				emit(url)
			}
		case <-rescan.C:
			selectLog()
		case event, ok := <-events:
			if !ok {
				events = nil
				continue
			}
			match, _ := filepath.Match("output_log_*.txt", filepath.Base(event.Name))
			if match && event.Op&(fsnotify.Create|fsnotify.Write|fsnotify.Rename) != 0 {
				selectLog()
			}
		case err, ok := <-errors:
			if !ok {
				errors = nil
			} else {
				log.Printf("Log watcher: %v", err)
			}
		}
	}
}
