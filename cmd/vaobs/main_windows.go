// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"log"
	"os"
	"path/filepath"
)

func main() {
	path, err := settingsPath()
	if err != nil {
		showError(0, err.Error())
		return
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		showError(0, err.Error())
		return
	}
	file, err := os.OpenFile(filepath.Join(filepath.Dir(path), "vaobs.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err == nil {
		defer file.Close()
		log.SetOutput(file)
	}
	settings, firstRun, loadErr := loadSettings(path)
	if loadErr != nil {
		log.Print(loadErr)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if err := runTray(ctx, cancel, path, settings, firstRun, loadErr); err != nil {
		showError(0, err.Error())
	}
}
