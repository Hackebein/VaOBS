// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"crypto/sha256"
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
	"unsafe"
)

func TestTraySettingsSaveCancelAndSingleInstance(t *testing.T) {
	t.Setenv("USERPROFILE", t.TempDir())
	address, calls := mockOBSServer(t, baseReply)
	config := serverSettings(t, address)
	path := filepath.Join(t.TempDir(), "settings.json")
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	var runErr error
	go func() { defer close(done); runErr = runTray(ctx, cancel, path, config, true, nil) }()
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(8 * time.Second):
			t.Error("tray did not quit")
		}
	})
	identity := fmt.Sprintf("%x", sha256.Sum256([]byte(strings.ToLower(path))))[:16]
	className := "VaOBSTray-" + identity
	find := func(title string) uintptr {
		value, _, _ := user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(utf16(className))), uintptr(unsafe.Pointer(utf16(title))))
		return value
	}
	deadline := time.Now().Add(5 * time.Second)
	var window uintptr
	for {
		select {
		case <-done:
			if runErr != nil && strings.Contains(runErr.Error(), "add tray icon") && os.Getenv("CI") != "" {
				t.Skip("notification area unavailable on this runner")
			}
			t.Fatalf("tray exited early: %v", runErr)
		default:
		}
		if time.Now().After(deadline) {
			t.Fatal("Settings did not open on first launch")
		}
		window = find("VaOBS Settings")
		if visible, _, _ := user32.NewProc("IsWindowVisible").Call(window); window != 0 && visible != 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	field := func(id int) uintptr {
		handle, _, _ := user32.NewProc("GetDlgItem").Call(window, uintptr(100+id))
		return handle
	}
	if windowText(field(3)) != "VRChatFeed" || windowText(field(4)) != "VRChatAudio" {
		t.Fatal("default source fields are missing")
	}
	if len(calls()) != 0 {
		t.Fatal("first launch contacted OBS before Settings was saved")
	}
	if character, _, _ := sendMessage.Call(field(2), 0xD2, 0, 0); character == 0 {
		t.Fatal("OBS password field is not masked")
	}
	setText.Call(field(4), uintptr(unsafe.Pointer(utf16("My VRChat Audio"))))
	setText.Call(field(2), uintptr(unsafe.Pointer(utf16("test-secret"))))
	sendMessage.Call(window, 0x111, 1, 0)
	loaded, first, err := loadSettings(path)
	if err != nil || first || loaded.ReferenceSource != "My VRChat Audio" || loaded.Password != "test-secret" {
		t.Fatalf("Save failed: %#v %v", loaded, err)
	}
	if err := runTray(context.Background(), func() {}, path, loaded, false, nil); err != nil {
		t.Fatal(err)
	}
	// The second launch opens the existing instance's Settings.
	postMessage.Call(find("VaOBS"), wmSettings, 0, 0)
	statusLabel, _, _ := user32.NewProc("GetDlgItem").Call(window, 200)
	deadline = time.Now().Add(5 * time.Second)
	for !strings.HasPrefix(windowText(statusLabel), "Connected") {
		if time.Now().After(deadline) {
			t.Fatal("Save did not connect and enable alignment")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if windowText(field(4)) != "My VRChat Audio" {
		t.Fatal("reopened Settings did not retain the saved source")
	}
	if path := os.Getenv("VAOBS_UI_SCREENSHOT"); path != "" {
		if err := captureSettingsWindow(window, path); err != nil {
			t.Fatal(err)
		}
	}
	setText.Call(field(4), uintptr(unsafe.Pointer(utf16("Unsaved draft"))))
	sendMessage.Call(window, 0x111, 2, 0)
	loaded, _, err = loadSettings(path)
	if err != nil || loaded.ReferenceSource != "My VRChat Audio" {
		t.Fatal("Cancel changed the saved source")
	}
	postMessage.Call(find("VaOBS"), 0x10, 0, 0)
	select {
	case <-done:
		if runErr != nil {
			t.Fatal(runErr)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Quit did not stop the tray app")
	}
}

// Capture only the app's own test window for visual verification.
func captureSettingsWindow(window uintptr, path string) error {
	var rect winRect
	user32.NewProc("GetWindowRect").Call(window, uintptr(unsafe.Pointer(&rect)))
	width, height := rect.Right-rect.Left, rect.Bottom-rect.Top
	dc, _, _ := user32.NewProc("GetWindowDC").Call(window)
	defer user32.NewProc("ReleaseDC").Call(window, dc)
	memoryDC, _, _ := gdi32.NewProc("CreateCompatibleDC").Call(dc)
	defer gdi32.NewProc("DeleteDC").Call(memoryDC)
	bitmap, _, _ := gdi32.NewProc("CreateCompatibleBitmap").Call(dc, uintptr(width), uintptr(height))
	defer gdi32.NewProc("DeleteObject").Call(bitmap)
	previous, _, _ := gdi32.NewProc("SelectObject").Call(memoryDC, bitmap)
	if ok, _, err := user32.NewProc("PrintWindow").Call(window, memoryDC, 2); ok == 0 {
		return err
	}
	gdi32.NewProc("SelectObject").Call(memoryDC, previous)
	type bitmapInfo struct {
		Size                   uint32
		Width, Height          int32
		Planes, Bits           uint16
		Compression, ImageSize uint32
		XPixels, YPixels       int32
		Colors, Important      uint32
		Color                  uint32
	}
	info := bitmapInfo{Size: 40, Width: width, Height: -height, Planes: 1, Bits: 32}
	pixels := make([]byte, int(width)*int(height)*4)
	if lines, _, err := gdi32.NewProc("GetDIBits").Call(memoryDC, bitmap, 0, uintptr(height), uintptr(unsafe.Pointer(&pixels[0])), uintptr(unsafe.Pointer(&info)), 0); lines == 0 {
		return err
	}
	for i := 0; i < len(pixels); i += 4 {
		pixels[i], pixels[i+2], pixels[i+3] = pixels[i+2], pixels[i], 255
	}
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	defer file.Close()
	return png.Encode(file, &image.RGBA{Pix: pixels, Stride: int(width) * 4, Rect: image.Rect(0, 0, int(width), int(height))})
}
