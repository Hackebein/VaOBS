// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestSettingsRoundTripProtectsPassword(t *testing.T) {
	path := filepath.Join(t.TempDir(), "VaOBS", "settings.json")
	settings, first, err := loadSettings(path)
	if err != nil || !first || settings != defaultSettings() {
		t.Fatalf("first launch: %v %v %#v", err, first, settings)
	}
	settings.Password = "private OBS password ä!"
	settings.ReferenceSource = "VRChat Audio Custom"
	for i := 0; i < 2; i++ {
		settings.Port = 4455 + i
		if err := saveSettings(path, settings); err != nil {
			t.Fatal(err)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if bytes.Contains(data, []byte(settings.Password)) || !bytes.Contains(data, []byte("password_dpapi")) {
			t.Fatal("password was not protected")
		}
		loaded, first, err := loadSettings(path)
		if err != nil || first || loaded != settings {
			t.Fatalf("round trip: %v %v %#v", err, first, loaded)
		}
	}
}

func TestInvalidSettingsDoNotReplaceSavedSettings(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.json")
	settings := defaultSettings()
	if err := saveSettings(path, settings); err != nil {
		t.Fatal(err)
	}
	bad := settings
	bad.ReferenceSource = bad.MediaSource
	if err := saveSettings(path, bad); err == nil {
		t.Fatal("accepted identical audio sources")
	}
	loaded, _, err := loadSettings(path)
	if err != nil || loaded != settings {
		t.Fatal("invalid settings replaced the saved configuration")
	}
	if err := os.WriteFile(path, []byte("{broken"), 0600); err != nil {
		t.Fatal(err)
	}
	loaded, first, err := loadSettings(path)
	if err == nil || !first || loaded != defaultSettings() {
		t.Fatal("corrupt settings should open setup with defaults")
	}
}

func TestSettingsValidation(t *testing.T) {
	for _, change := range []func(*Settings){
		func(s *Settings) { s.Port = 0 }, func(s *Settings) { s.Port = 65536 },
		func(s *Settings) { s.Host = "ws://localhost:4455" }, func(s *Settings) { s.MediaSource = "" },
		func(s *Settings) { s.MaxDelayMS = 121000 }, func(s *Settings) { s.ReferenceSource = s.OutputSource },
	} {
		s := defaultSettings()
		change(&s)
		if s.validate() == nil {
			t.Fatalf("invalid settings accepted: %#v", s)
		}
	}
}
