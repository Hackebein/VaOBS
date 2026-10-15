// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestControllerRetriesWhenOBSSetupBecomesAvailable(t *testing.T) {
	t.Setenv("USERPROFILE", t.TempDir())
	var attempted atomic.Bool
	address, _ := mockOBSServer(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "GetInputKindList" && !attempted.Swap(true) {
			return map[string]any{"inputKinds": []string{}}, 100
		}
		return baseReply(call)
	})
	status := make(chan string, 64)
	c := newController(func(message string) { status <- message })
	ctx, cancel := context.WithCancel(context.Background())
	go c.run(ctx, serverSettings(t, address), true)
	t.Cleanup(func() {
		cancel()
		select {
		case <-c.done:
		case <-time.After(5 * time.Second):
			t.Error("controller did not stop")
		}
	})
	failureSeen := false
	deadline := time.After(8 * time.Second)
	for {
		select {
		case message := <-status:
			if strings.HasPrefix(message, "OBS unavailable") {
				failureSeen = true
			}
			if strings.HasPrefix(message, "Connected") {
				if !failureSeen {
					t.Fatal("test did not exercise a failed setup")
				}
				return
			}
		case <-deadline:
			t.Fatal("OBS setup was not retried automatically")
		}
	}
}

func serverSettings(t *testing.T, address string) Settings {
	t.Helper()
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		t.Fatal(err)
	}
	number, err := strconv.Atoi(port)
	if err != nil {
		t.Fatal(err)
	}
	config := defaultSettings()
	config.Host, config.Port = host, number
	return config
}

func TestControllerAppliesSettingsAndAlwaysConfiguresAlignment(t *testing.T) {
	t.Setenv("USERPROFILE", t.TempDir())
	dir, err := getVRChatLogDir()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "output_log_test.txt"), []byte("[Video Playback] Resolving URL 'https://example.invalid/video'\n"), 0600); err != nil {
		t.Fatal(err)
	}
	address, calls := mockOBSServer(t, baseReply)
	config := serverSettings(t, address)
	status := make(chan string, 64)
	c := newController(func(message string) { status <- message })
	ctx, cancel := context.WithCancel(context.Background())
	go c.run(ctx, config, true)
	t.Cleanup(func() {
		cancel()
		select {
		case <-c.done:
		case <-time.After(5 * time.Second):
			t.Error("controller did not stop")
		}
	})
	waitConnected := func() {
		t.Helper()
		deadline := time.After(5 * time.Second)
		for {
			select {
			case message := <-status:
				if strings.HasPrefix(message, "Connected") {
					return
				}
			case <-deadline:
				t.Fatal("controller did not connect")
			}
		}
	}
	waitConnected()
	config.ReferenceSource, config.OutputSource = "OtherVRChatAudio", "OtherAlignedOutput"
	replaceLatest(c.settings, config)
	waitConnected()
	configured := false
	retired := false
	for _, call := range calls() {
		if call.RequestType == "CreateInput" && call.Data["inputName"] == config.OutputSource {
			settings := call.Data["inputSettings"].(map[string]any)
			configured = settings["reference_source"] == config.ReferenceSource && settings["feed_source"] == config.MediaSource
		}
		if call.RequestType == "SetInputMute" && call.Data["inputName"] == "VaOBSAlignedAudio" && call.Data["inputMuted"] == true {
			retired = true
		}
	}
	if !configured || !retired {
		t.Fatalf("settings were not applied without duplicate output: %#v", calls())
	}
}

func TestLogPlaybackAlwaysResetsAlignmentAndKeepsPR1MediaSettings(t *testing.T) {
	client, calls := mockOBS(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "GetInputSettings" {
			return map[string]any{"inputSettings": map[string]any{"restart_on_activate": false}}, 100
		}
		return baseReply(call)
	})
	if err := updateMedia(client, defaultSettings(), "rtspt://example.invalid/live", 8); err != nil {
		t.Fatal(err)
	}
	history := calls()
	if len(history) != 3 || history[1].Data["inputName"] != "VaOBSAlignedAudio" || history[2].Data["inputName"] != "VRChatFeed" {
		t.Fatalf("wrong playback/reset order: %#v", history)
	}
	reset := history[1].Data["inputSettings"].(map[string]any)
	media := history[2].Data["inputSettings"].(map[string]any)
	if reset["stream_generation"] != float64(8) || media["input"] != "rtmp://example.invalid/live" || media["seekable"] != false || media["is_local_file"] != false || media["restart_on_activate"] != false {
		t.Fatalf("incorrect merged settings: %#v %#v", reset, media)
	}
}

func TestRenamedAlignedOutputDoesNotInheritMutedFeed(t *testing.T) {
	client, calls := mockOBS(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "GetInputMute" {
			return map[string]any{"inputMuted": true}, 100
		}
		return baseReply(call)
	})
	config := testConfig()
	config.Output = "RenamedAlignedOutput"
	muted := false
	config.InitialMuted = &muted
	if err := configureAlignment(client, config, 1); err != nil {
		t.Fatal(err)
	}
	for _, call := range calls() {
		if call.RequestType == "SetInputMute" && call.Data["inputName"] == config.Output {
			if call.Data["inputMuted"] != false {
				t.Fatal("new output inherited the application's muted feed state")
			}
			return
		}
	}
	t.Fatal("new output mute state was not configured")
}
