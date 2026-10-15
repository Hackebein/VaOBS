// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"crypto/sha256"
	_ "embed"
	"encoding/binary"
	"fmt"
	"log"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"unsafe"

	"golang.org/x/sys/windows"
)

//go:embed tray.ico
var trayIcon []byte

var (
	user32           = windows.NewLazySystemDLL("user32.dll")
	shell32          = windows.NewLazySystemDLL("shell32.dll")
	gdi32            = windows.NewLazySystemDLL("gdi32.dll")
	kernel32         = windows.NewLazySystemDLL("kernel32.dll")
	createWindow     = user32.NewProc("CreateWindowExW")
	sendMessage      = user32.NewProc("SendMessageW")
	postMessage      = user32.NewProc("PostMessageW")
	setText          = user32.NewProc("SetWindowTextW")
	showWindow       = user32.NewProc("ShowWindow")
	foregroundWindow = user32.NewProc("SetForegroundWindow")
	notifyIcon       = shell32.NewProc("Shell_NotifyIconW")
	destroyWindow    = user32.NewProc("DestroyWindow")
	windowProc       = windows.NewCallback(trayWindowProc)
	activeTray       *trayApp // Owned by the locked Windows message-loop thread.
)

const (
	wmTray        = 0x8001
	wmSettings    = 0x8002
	wmStatus      = 0x8003
	wmQuitReady   = 0x8004
	menuSettings  = 10
	menuReconnect = 11
	menuLog       = 12
	menuQuit      = 13
)

type winPoint struct{ X, Y int32 }
type winRect struct{ Left, Top, Right, Bottom int32 }
type winMessage struct {
	Window         uintptr
	Message        uint32
	WParam, LParam uintptr
	Time           uint32
	Point          winPoint
	Private        uint32
}
type winClass struct {
	Size, Style                        uint32
	Proc                               uintptr
	ClassExtra, WindowExtra            int32
	Instance, Icon, Cursor, Background uintptr
	MenuName, ClassName                *uint16
	SmallIcon                          uintptr
}
type notifyData struct {
	Size                uint32
	Window              uintptr
	ID, Flags, Callback uint32
	Icon                uintptr
	Tip                 [128]uint16
	State, StateMask    uint32
	Info                [256]uint16
	Version             uint32
	Title               [64]uint16
	InfoFlags           uint32
	GUID                windows.GUID
	BalloonIcon         uintptr
}

type trayApp struct {
	window, settingsWindow, icon, font, instance uintptr
	className                                    string
	dpi                                          int
	taskbarCreated                               uint32
	config                                       Settings
	configPath                                   string
	fields                                       [6]uintptr
	statusLabel                                  uintptr
	controller                                   *controller
	cancel                                       context.CancelFunc
	quitting                                     bool
	statusMutex                                  sync.Mutex
	status                                       string
}

func utf16(text string) *uint16 { return windows.StringToUTF16Ptr(text) }

func showError(owner uintptr, text string) {
	user32.NewProc("MessageBoxW").Call(owner, uintptr(unsafe.Pointer(utf16(text))), uintptr(unsafe.Pointer(utf16("VaOBS"))), 0x10)
}

func limitText(text string, max int) string {
	runes := []rune(text)
	if len(runes) > max {
		return string(runes[:max-3]) + "..."
	}
	return text
}

func (t *trayApp) updateStatus(status string) {
	t.statusMutex.Lock()
	changed := t.status != status
	t.status = status
	t.statusMutex.Unlock()
	if changed {
		log.Print(status)
		postMessage.Call(t.window, wmStatus, 0, 0)
	}
}

func (t *trayApp) currentStatus() string {
	t.statusMutex.Lock()
	defer t.statusMutex.Unlock()
	return t.status
}

func (t *trayApp) notification() notifyData {
	data := notifyData{Window: t.window, ID: 1, Flags: 1 | 2 | 4 | 0x80, Callback: wmTray, Icon: t.icon}
	data.Size = uint32(unsafe.Sizeof(data))
	copy(data.Tip[:], windows.StringToUTF16(limitText("VaOBS: "+t.currentStatus(), 120)))
	return data
}

func (t *trayApp) addIcon() error {
	data := t.notification()
	if ok, _, err := notifyIcon.Call(0, uintptr(unsafe.Pointer(&data))); ok == 0 {
		return fmt.Errorf("add tray icon: %v", err)
	}
	data.Version = 4
	notifyIcon.Call(4, uintptr(unsafe.Pointer(&data)))
	return nil
}

func (t *trayApp) scale(n int) int { return n * t.dpi / 96 }

func (t *trayApp) createSettingsWindow() error {
	const style = 0x00CA0000 // caption, system menu, minimize; fixed client size
	rect := winRect{Right: int32(t.scale(560)), Bottom: int32(t.scale(420))}
	user32.NewProc("AdjustWindowRectEx").Call(uintptr(unsafe.Pointer(&rect)), style, 0, 0x10001)
	width, height := int(rect.Right-rect.Left), int(rect.Bottom-rect.Top)
	var work winRect
	user32.NewProc("SystemParametersInfoW").Call(48, 0, uintptr(unsafe.Pointer(&work)), 0)
	x, y := int(work.Left)+(int(work.Right-work.Left)-width)/2, int(work.Top)+(int(work.Bottom-work.Top)-height)/2
	window, _, err := createWindow.Call(0x10001, uintptr(unsafe.Pointer(utf16(t.className))), uintptr(unsafe.Pointer(utf16("VaOBS Settings"))), style,
		uintptr(x), uintptr(y), uintptr(width), uintptr(height), 0, 0, t.instance, 0)
	if window == 0 {
		return fmt.Errorf("open Settings: %v", err)
	}
	t.settingsWindow = window
	sendMessage.Call(window, 0x80, 0, t.icon)
	sendMessage.Call(window, 0x80, 1, t.icon)
	makeControl := func(class, text string, style uint32, x, y, w, h, id int) uintptr {
		handle, _, _ := createWindow.Call(0, uintptr(unsafe.Pointer(utf16(class))), uintptr(unsafe.Pointer(utf16(text))), uintptr(style|0x50000000),
			uintptr(t.scale(x)), uintptr(t.scale(y)), uintptr(t.scale(w)), uintptr(t.scale(h)), window, uintptr(id), t.instance, 0)
		sendMessage.Call(handle, 0x30, t.font, 1)
		return handle
	}
	t.statusLabel = makeControl("STATIC", t.currentStatus(), 0, 20, 16, 520, 44, 200)
	labels := []string{"OBS host", "OBS port", "OBS password", "Media source to sync", "Timing reference", "Delay limit (seconds)"}
	for i, label := range labels {
		y := 78 + i*43
		if i >= 3 {
			y += 16
		}
		makeControl("STATIC", label, 0, 20, y+4, 172, 24, 0)
		style := uint32(0x00810080) // border, tab stop, horizontal auto-scroll
		if i == 2 {
			style |= 0x20
		} // ES_PASSWORD
		if i == 1 || i == 5 {
			style |= 0x2000
		} // ES_NUMBER
		t.fields[i] = makeControl("EDIT", "", style, 200, y, 340, 29, 100+i)
		sendMessage.Call(t.fields[i], 0xC5, 1024, 0) // EM_LIMITTEXT
	}
	makeControl("STATIC", "Save applies changes and reconnects to OBS.", 0, 20, 354, 520, 22, 0)
	makeControl("BUTTON", "Save", 0x00010001, 348, 386, 90, 28, 1)
	makeControl("BUTTON", "Cancel", 0x00010000, 450, 386, 90, 28, 2)
	for _, field := range t.fields {
		if field == 0 {
			return fmt.Errorf("could not create a Settings field")
		}
	}
	return nil
}

func (t *trayApp) openSettings() {
	if t.quitting {
		return
	}
	if t.settingsWindow == 0 {
		if err := t.createSettingsWindow(); err != nil {
			showError(t.window, err.Error())
			return
		}
	}
	values := []string{t.config.Host, strconv.Itoa(t.config.Port), t.config.Password, t.config.MediaSource,
		t.config.ReferenceSource, strconv.Itoa(t.config.MaxDelayMS / 1000)}
	for i, value := range values {
		setText.Call(t.fields[i], uintptr(unsafe.Pointer(utf16(value))))
	}
	setText.Call(t.statusLabel, uintptr(unsafe.Pointer(utf16(t.currentStatus()))))
	showWindow.Call(t.settingsWindow, 9) // restore from minimized state
	foregroundWindow.Call(t.settingsWindow)
	user32.NewProc("SetFocus").Call(t.fields[0])
}

func windowText(window uintptr) string {
	length, _, _ := user32.NewProc("GetWindowTextLengthW").Call(window)
	buffer := make([]uint16, length+1)
	user32.NewProc("GetWindowTextW").Call(window, uintptr(unsafe.Pointer(&buffer[0])), uintptr(len(buffer)))
	return windows.UTF16ToString(buffer)
}

func (t *trayApp) save() {
	if t.quitting {
		return
	}
	port, err := strconv.Atoi(windowText(t.fields[1]))
	if err != nil {
		showError(t.settingsWindow, "Enter a valid OBS port.")
		return
	}
	seconds, err := strconv.Atoi(windowText(t.fields[5]))
	if err != nil || seconds < 1 || seconds > 120 {
		showError(t.settingsWindow, "Maximum delay must be between 1 and 120 seconds.")
		return
	}
	config := Settings{Host: strings.TrimSpace(windowText(t.fields[0])), Port: port,
		Password: windowText(t.fields[2]), MediaSource: strings.TrimSpace(windowText(t.fields[3])),
		ReferenceSource: strings.TrimSpace(windowText(t.fields[4])), OutputSource: t.config.OutputSource, MaxDelayMS: seconds * 1000}
	if err := saveSettings(t.configPath, config); err != nil {
		showError(t.settingsWindow, err.Error())
		return
	}
	t.config = config
	replaceLatest(t.controller.settings, config)
	showWindow.Call(t.settingsWindow, 0)
}

func (t *trayApp) openMenu() {
	menu, _, _ := user32.NewProc("CreatePopupMenu").Call()
	if menu == 0 {
		return
	}
	defer user32.NewProc("DestroyMenu").Call(menu)
	appendMenu := user32.NewProc("AppendMenuW")
	appendMenu.Call(menu, 1, 0, uintptr(unsafe.Pointer(utf16(limitText(t.currentStatus(), 100)))))
	appendMenu.Call(menu, 0x800, 0, 0)
	for _, item := range []struct {
		id    uintptr
		title string
	}{{menuSettings, "Settings..."}, {menuReconnect, "Reconnect"}, {menuLog, "Open log"}, {menuQuit, "Quit"}} {
		appendMenu.Call(menu, 0, item.id, uintptr(unsafe.Pointer(utf16(item.title))))
	}
	var point winPoint
	user32.NewProc("GetCursorPos").Call(uintptr(unsafe.Pointer(&point)))
	foregroundWindow.Call(t.window)
	command, _, _ := user32.NewProc("TrackPopupMenu").Call(menu, 0x102, uintptr(point.X), uintptr(point.Y), 0, t.window, 0)
	postMessage.Call(t.window, 0, 0, 0)
	switch command {
	case menuSettings:
		t.openSettings()
	case menuReconnect:
		select {
		case t.controller.reconnect <- struct{}{}:
		default:
		}
	case menuLog:
		shell32.NewProc("ShellExecuteW").Call(t.window, uintptr(unsafe.Pointer(utf16("open"))), uintptr(unsafe.Pointer(utf16(filepath.Join(filepath.Dir(t.configPath), "vaobs.log")))), 0, 0, 1)
	case menuQuit:
		t.quit()
	}
}

func (t *trayApp) quit() {
	if t.quitting {
		return
	}
	t.quitting = true
	t.updateStatus("Closing...")
	t.cancel()
	go func() { <-t.controller.done; postMessage.Call(t.window, wmQuitReady, 0, 0) }()
}

func trayWindowProc(window uintptr, message uint32, wParam, lParam uintptr) uintptr {
	t := activeTray
	if t != nil {
		if message == t.taskbarCreated && window == t.window {
			_ = t.addIcon()
			return 0
		}
		switch message {
		case wmTray:
			switch uint16(lParam) {
			case 0x7B, 0x400, 0x401:
				t.openMenu()
			case 0x203:
				t.openSettings()
			}
			return 0
		case wmSettings:
			t.openSettings()
			return 0
		case wmStatus:
			data := t.notification()
			notifyIcon.Call(1, uintptr(unsafe.Pointer(&data)))
			if t.statusLabel != 0 {
				setText.Call(t.statusLabel, uintptr(unsafe.Pointer(utf16(t.currentStatus()))))
			}
			return 0
		case wmQuitReady:
			destroyWindow.Call(t.window)
			return 0
		case 0x111: // WM_COMMAND
			if window == t.settingsWindow {
				switch uint16(wParam) {
				case 1:
					t.save()
				case 2:
					showWindow.Call(window, 0)
				}
				return 0
			}
		case 0x10: // WM_CLOSE
			if window == t.settingsWindow {
				showWindow.Call(window, 0)
			} else {
				t.quit()
			}
			return 0
		case 0x11:
			return 1 // WM_QUERYENDSESSION
		case 0x16:
			if wParam != 0 {
				t.quit()
			}
			return 0
		case 2: // WM_DESTROY
			if window == t.window {
				user32.NewProc("PostQuitMessage").Call(0)
				return 0
			}
		}
	}
	result, _, _ := user32.NewProc("DefWindowProcW").Call(window, uintptr(message), wParam, lParam)
	return result
}

func runTray(ctx context.Context, cancel context.CancelFunc, path string, config Settings, firstRun bool, loadErr error) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	identity := fmt.Sprintf("%x", sha256.Sum256([]byte(strings.ToLower(path))))[:16]
	className := "VaOBSTray-" + identity
	mutex, _, lastError := kernel32.NewProc("CreateMutexW").Call(0, 0, uintptr(unsafe.Pointer(utf16("Local\\"+className))))
	if mutex == 0 {
		return fmt.Errorf("create app instance: %v", lastError)
	}
	defer windows.CloseHandle(windows.Handle(mutex))
	if lastError == windows.ERROR_ALREADY_EXISTS {
		window, _, _ := user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(utf16(className))), 0)
		if window != 0 {
			postMessage.Call(window, wmSettings, 0, 0)
		}
		return nil
	}
	user32.NewProc("SetProcessDPIAware").Call()
	instance, _, _ := kernel32.NewProc("GetModuleHandleW").Call(0)
	t := &trayApp{className: className, instance: instance, config: config, configPath: path,
		cancel: cancel, dpi: 96, status: "Starting automatic alignment..."}
	if dpi := user32.NewProc("GetDpiForSystem"); dpi.Find() == nil {
		value, _, _ := dpi.Call()
		if value != 0 {
			t.dpi = int(value)
		}
	}
	iconOffset := binary.LittleEndian.Uint32(trayIcon[18:22])
	iconBytes := trayIcon[iconOffset:]
	t.icon, _, _ = user32.NewProc("CreateIconFromResourceEx").Call(uintptr(unsafe.Pointer(&iconBytes[0])), uintptr(len(iconBytes)), 1, 0x30000, 32, 32, 0)
	if t.icon == 0 {
		t.icon, _, _ = user32.NewProc("LoadIconW").Call(0, 32512)
	} else {
		defer user32.NewProc("DestroyIcon").Call(t.icon)
	}
	height := int32(-t.scale(15))
	t.font, _, _ = gdi32.NewProc("CreateFontW").Call(uintptr(height), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, uintptr(unsafe.Pointer(utf16("Segoe UI"))))
	if t.font != 0 {
		defer gdi32.NewProc("DeleteObject").Call(t.font)
	}
	cursor, _, _ := user32.NewProc("LoadCursorW").Call(0, 32512)
	class := winClass{Proc: windowProc, Instance: instance, Icon: t.icon, SmallIcon: t.icon, Cursor: cursor, Background: 16, ClassName: utf16(className)}
	class.Size = uint32(unsafe.Sizeof(class))
	if atom, _, err := user32.NewProc("RegisterClassExW").Call(uintptr(unsafe.Pointer(&class))); atom == 0 {
		return fmt.Errorf("register tray window: %v", err)
	}
	defer user32.NewProc("UnregisterClassW").Call(uintptr(unsafe.Pointer(utf16(className))), instance)
	activeTray = t
	defer func() { activeTray = nil }()
	t.window, _, lastError = createWindow.Call(0, uintptr(unsafe.Pointer(utf16(className))), uintptr(unsafe.Pointer(utf16("VaOBS"))), 0, 0, 0, 0, 0, 0, 0, instance, 0)
	if t.window == 0 {
		return fmt.Errorf("create tray window: %v", lastError)
	}
	defer destroyWindow.Call(t.window)
	taskbar, _, _ := user32.NewProc("RegisterWindowMessageW").Call(uintptr(unsafe.Pointer(utf16("TaskbarCreated"))))
	t.taskbarCreated = uint32(taskbar)
	if err := t.addIcon(); err != nil {
		return err
	}
	defer func() {
		data := t.notification()
		notifyIcon.Call(2, uintptr(unsafe.Pointer(&data)))
		if t.settingsWindow != 0 {
			destroyWindow.Call(t.settingsWindow)
		}
	}()
	t.controller = newController(t.updateStatus)
	go t.controller.run(ctx, config, !firstRun && loadErr == nil)
	go func() { <-ctx.Done(); postMessage.Call(t.window, 0x10, 0, 0) }()
	defer func() { cancel(); <-t.controller.done }()
	if firstRun {
		t.openSettings()
	}
	if loadErr != nil {
		showError(t.settingsWindow, loadErr.Error())
	}
	var message winMessage
	for {
		result, _, err := user32.NewProc("GetMessageW").Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
		if int32(result) == -1 {
			return fmt.Errorf("read window messages: %v", err)
		}
		if result == 0 {
			return nil
		}
		if t.settingsWindow != 0 {
			if message.Message == 0x100 && message.WParam == 0x1B {
				showWindow.Call(t.settingsWindow, 0)
				continue
			}
			if message.Message == 0x100 && message.WParam == 0x0D {
				t.save()
				continue
			}
			if handled, _, _ := user32.NewProc("IsDialogMessageW").Call(t.settingsWindow, uintptr(unsafe.Pointer(&message))); handled != 0 {
				continue
			}
		}
		user32.NewProc("TranslateMessage").Call(uintptr(unsafe.Pointer(&message)))
		user32.NewProc("DispatchMessageW").Call(uintptr(unsafe.Pointer(&message)))
	}
}
