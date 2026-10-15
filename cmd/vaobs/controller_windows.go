// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"
	"time"

	"github.com/andreykaipov/goobs"
	"github.com/andreykaipov/goobs/api/requests/inputs"
	"github.com/gorilla/websocket"
)

type controller struct {
	settings  chan Settings
	reconnect chan struct{}
	urls      chan string
	done      chan struct{}
	status    func(string)
}

type obsLogger struct{}

func (obsLogger) Printf(format string, values ...any) {
	if !strings.HasPrefix(format, "[TRACE]") && !strings.HasPrefix(format, "[DEBUG]") {
		log.Printf(format, values...)
	}
}

func newController(status func(string)) *controller {
	return &controller{settings: make(chan Settings, 1), reconnect: make(chan struct{}, 1),
		urls: make(chan string, 1), done: make(chan struct{}), status: status}
}

func replaceLatest[T any](channel chan T, value T) {
	select {
	case channel <- value:
		return
	default:
	}
	select {
	case <-channel:
	default:
	}
	channel <- value
}

func updateMedia(client *goobs.Client, config Settings, url string, generation int64) error {
	if strings.HasPrefix(url, "rtspt://") {
		url = "rtmp://" + strings.TrimPrefix(url, "rtspt://")
	}
	settings, err := client.Inputs.GetInputSettings(inputs.NewGetInputSettingsParams().WithInputName(config.MediaSource))
	if err != nil {
		return err
	}
	if err := resetAlignment(client, config.OutputSource, generation); err != nil {
		return err
	}
	media := settings.InputSettings
	if media == nil {
		media = map[string]any{}
	}
	media["input"], media["is_local_file"], media["seekable"] = url, false, false
	_, err = client.Inputs.SetInputSettings(inputs.NewSetInputSettingsParams().WithInputName(config.MediaSource).
		WithInputSettings(media).WithOverlay(false))
	return err
}

func (c *controller) run(ctx context.Context, config Settings, ready bool) {
	defer close(c.done)
	var client *goobs.Client
	var events <-chan any
	var generation int64
	var initialMuted *bool
	var retiredOutput string
	var url string
	haveURL := false
	watchCtx, stopWatch := context.WithCancel(ctx)
	watchDone := make(chan struct{})
	if dir, err := getVRChatLogDir(); err == nil {
		go func() {
			defer close(watchDone)
			watchLogs(watchCtx, dir, func(url string) { replaceLatest(c.urls, url) })
		}()
	} else {
		close(watchDone)
	}
	disconnect := func(clear bool) {
		if client != nil {
			if clear {
				generation++
				_ = updateMedia(client, config, "", generation)
			}
			_ = client.Disconnect()
			client, events = nil, nil
		}
	}
	defer func() { stopWatch(); <-watchDone; disconnect(true) }()
	retry := time.NewTicker(5 * time.Second)
	defer retry.Stop()
	connect := func() {
		if client != nil || ctx.Err() != nil || !ready {
			return
		}
		c.status("Connecting to OBS...")
		dialer := *websocket.DefaultDialer
		dialer.HandshakeTimeout = 3 * time.Second
		var transport net.Conn
		dialer.NetDialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
			connection, err := (&net.Dialer{Timeout: 3 * time.Second}).DialContext(ctx, network, address)
			transport = connection
			return connection, err
		}
		var err error
		client, err = goobs.New(net.JoinHostPort(strings.Trim(config.Host, "[]"), strconv.Itoa(config.Port)),
			goobs.WithPassword(config.Password), goobs.WithResponseTimeout(1500),
			goobs.WithDialer(&dialer), goobs.WithLogger(obsLogger{}), goobs.WithEventSubscriptions(0))
		if err != nil && transport != nil {
			_ = transport.Close()
		}
		if err == nil && ctx.Err() == nil {
			if retiredOutput != "" {
				_, _ = client.Inputs.SetInputMute(inputs.NewSetInputMuteParams().WithInputName(retiredOutput).WithInputMuted(true))
			}
			generation++
			alignment := config.alignment()
			alignment.InitialMuted = initialMuted
			err = configureAlignment(client, alignment, generation)
			if err == nil {
				initialMuted, retiredOutput = nil, ""
			}
		}
		if err == nil && haveURL && ctx.Err() == nil {
			generation++
			err = updateMedia(client, config, url, generation)
		}
		if err != nil {
			disconnect(false)
			c.status(fmt.Sprintf("OBS unavailable: %v", err))
			return
		}
		if client != nil {
			events = client.IncomingEvents
			c.status("Connected to OBS")
		}
	}
	if ready {
		connect()
	} else {
		c.status("Open Settings to connect to OBS")
	}
	for {
		if ctx.Err() != nil {
			return
		}
		select {
		case <-ctx.Done():
			return
		case next := <-c.settings:
			ready = true
			if !strings.EqualFold(next.Host, config.Host) || next.Port != config.Port {
				retiredOutput, initialMuted = "", nil
			}
			if next.OutputSource != config.OutputSource {
				muted := false
				if client != nil {
					if result, err := client.Inputs.GetInputMute(inputs.NewGetInputMuteParams().WithInputName(config.OutputSource)); err == nil {
						muted = result.InputMuted
					}
					_, _ = client.Inputs.SetInputMute(inputs.NewSetInputMuteParams().WithInputName(config.OutputSource).WithInputMuted(true))
				}
				initialMuted = &muted
				if strings.EqualFold(next.Host, config.Host) && next.Port == config.Port {
					retiredOutput = config.OutputSource
				}
			}
			disconnect(true)
			config = next
			connect()
		case <-c.reconnect:
			disconnect(false)
			connect()
		case next := <-c.urls:
			url, haveURL = next, true
			if client != nil {
				generation++
				if err := updateMedia(client, config, url, generation); err != nil {
					c.status(fmt.Sprintf("OBS update failed: %v", err))
					disconnect(false)
				}
			}
		case _, ok := <-events:
			if !ok {
				disconnect(false)
				c.status("OBS disconnected - retrying automatically")
			}
		case <-retry.C:
			connect()
		}
	}
}
