// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

type Settings struct {
	Host            string `json:"host"`
	Port            int    `json:"port"`
	Password        string `json:"-"`
	MediaSource     string `json:"media_source"`
	ReferenceSource string `json:"reference_source"`
	OutputSource    string `json:"output_source"` // Managed output; retain names saved by earlier builds.
	MaxDelayMS      int    `json:"max_delay_ms"`
}

func defaultSettings() Settings {
	return Settings{Host: "localhost", Port: 4455, MediaSource: "VRChatFeed",
		ReferenceSource: "VRChatAudio", OutputSource: "VaOBSAlignedAudio", MaxDelayMS: 60000}
}

func (s Settings) alignment() alignmentConfig {
	return alignmentConfig{Source: s.MediaSource, Reference: s.ReferenceSource, Output: s.OutputSource, MaxDelayMS: s.MaxDelayMS}
}

func (s Settings) validate() error {
	if strings.TrimSpace(s.Host) == "" || strings.ContainsAny(s.Host, " /\\\t\r\n\x00") || strings.Contains(s.Host, "://") {
		return fmt.Errorf("enter an OBS host name or IP address, without a URL or port")
	}
	if strings.Contains(s.Host, ":") && net.ParseIP(strings.Trim(s.Host, "[]")) == nil {
		return fmt.Errorf("enter the OBS port in the separate port field")
	}
	if s.Port < 1 || s.Port > 65535 {
		return fmt.Errorf("OBS port must be between 1 and 65535")
	}
	for _, value := range []string{s.Password, s.MediaSource, s.ReferenceSource, s.OutputSource} {
		if strings.ContainsRune(value, 0) {
			return fmt.Errorf("settings cannot contain a null character")
		}
	}
	return s.alignment().validate()
}

func settingsPath() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "VaOBS", "settings.json"), nil
}

type storedSettings struct {
	Version int `json:"version"`
	Settings
	ProtectedPassword string `json:"password_dpapi,omitempty"`
}

func loadSettings(path string) (Settings, bool, error) {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return defaultSettings(), true, nil
	}
	if err != nil {
		return defaultSettings(), true, err
	}
	stored := storedSettings{Version: 1, Settings: defaultSettings()}
	if err := json.Unmarshal(data, &stored); err != nil {
		return defaultSettings(), true, fmt.Errorf("read saved settings: %w", err)
	}
	if stored.Version != 1 {
		return defaultSettings(), true, fmt.Errorf("unsupported settings version %d", stored.Version)
	}
	if err := stored.Settings.validate(); err != nil {
		return defaultSettings(), true, err
	}
	if stored.ProtectedPassword != "" {
		secret, err := base64.StdEncoding.DecodeString(stored.ProtectedPassword)
		if err == nil {
			secret, err = protectSecret(secret, false)
		}
		if err != nil {
			return stored.Settings, true, fmt.Errorf("Windows could not unlock the saved OBS password; enter it again")
		}
		stored.Password = string(secret)
	}
	return stored.Settings, false, nil
}

func saveSettings(path string, settings Settings) error {
	if err := settings.validate(); err != nil {
		return err
	}
	stored := storedSettings{Version: 1, Settings: settings}
	if settings.Password != "" {
		secret, err := protectSecret([]byte(settings.Password), true)
		if err != nil {
			return fmt.Errorf("protect OBS password: %w", err)
		}
		stored.ProtectedPassword = base64.StdEncoding.EncodeToString(secret)
	}
	data, err := json.MarshalIndent(stored, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(path), "settings-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := file.Write(append(data, '\n')); err != nil {
		file.Close()
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}

func protectSecret(data []byte, encrypt bool) ([]byte, error) {
	if len(data) == 0 {
		return nil, nil
	}
	input := windows.DataBlob{Size: uint32(len(data)), Data: &data[0]}
	var output windows.DataBlob
	var err error
	if encrypt {
		err = windows.CryptProtectData(&input, nil, nil, 0, nil, windows.CRYPTPROTECT_UI_FORBIDDEN, &output)
	} else {
		err = windows.CryptUnprotectData(&input, nil, nil, 0, nil, windows.CRYPTPROTECT_UI_FORBIDDEN, &output)
	}
	if err != nil {
		return nil, err
	}
	defer windows.LocalFree(windows.Handle(unsafe.Pointer(output.Data)))
	return append([]byte(nil), unsafe.Slice(output.Data, int(output.Size))...), nil
}
