// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/andreykaipov/goobs"
	"github.com/gorilla/websocket"
)

type obsCall struct {
	RequestType string         `json:"requestType"`
	RequestID   string         `json:"requestId"`
	Data        map[string]any `json:"requestData"`
}

func mockOBS(t *testing.T, reply func(obsCall) (map[string]any, int)) (*goobs.Client, func() []obsCall) {
	t.Helper()
	address, calls := mockOBSServer(t, reply)
	client, err := goobs.New(address)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { client.Disconnect() })
	return client, calls
}

func mockOBSServer(t *testing.T, reply func(obsCall) (map[string]any, int)) (string, func() []obsCall) {
	t.Helper()
	var calls []obsCall
	var mu sync.Mutex
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			t.Error(err)
			return
		}
		defer conn.Close()
		if err := conn.WriteJSON(map[string]any{"op": 0, "d": map[string]any{"obsWebSocketVersion": "5.5.0", "rpcVersion": 1}}); err != nil {
			return
		}
		var identify map[string]any
		if conn.ReadJSON(&identify) != nil {
			return
		}
		if conn.WriteJSON(map[string]any{"op": 2, "d": map[string]any{"negotiatedRpcVersion": 1}}) != nil {
			return
		}
		for {
			var request struct {
				Op   int             `json:"op"`
				Data json.RawMessage `json:"d"`
			}
			if conn.ReadJSON(&request) != nil {
				return
			}
			if request.Op != 6 {
				continue
			}
			var call obsCall
			if json.Unmarshal(request.Data, &call) != nil {
				return
			}
			mu.Lock()
			calls = append(calls, call)
			mu.Unlock()
			data, code := reply(call)
			if data == nil {
				data = map[string]any{}
			}
			response := map[string]any{"op": 7, "d": map[string]any{
				"requestType": call.RequestType, "requestId": call.RequestID,
				"requestStatus": map[string]any{"result": code == 100, "code": code}, "responseData": data,
			}}
			if conn.WriteJSON(response) != nil {
				return
			}
		}
	}))
	t.Cleanup(server.Close)
	return strings.TrimPrefix(server.URL, "http://"), func() []obsCall {
		mu.Lock()
		defer mu.Unlock()
		return append([]obsCall(nil), calls...)
	}
}

func baseReply(call obsCall) (map[string]any, int) {
	switch call.RequestType {
	case "GetInputKindList":
		return map[string]any{"inputKinds": []string{alignmentInputKind}}, 100
	case "GetInputSettings":
		return map[string]any{"inputKind": "ffmpeg_source", "inputSettings": map[string]any{}}, 100
	case "GetInputList":
		return map[string]any{"inputs": []any{}}, 100
	case "GetCurrentProgramScene":
		return map[string]any{"currentProgramSceneName": "VRChat"}, 100
	case "GetSceneItemList":
		return map[string]any{"sceneItems": []any{}}, 100
	case "GetInputAudioTracks":
		return map[string]any{"inputAudioTracks": map[string]bool{"1": false, "2": true, "3": false, "4": false, "5": false, "6": false}}, 100
	case "GetInputVolume":
		return map[string]any{"inputVolumeMul": .8}, 100
	case "GetInputMute":
		return map[string]any{"inputMuted": false}, 100
	default:
		return nil, 100
	}
}

func testConfig() alignmentConfig {
	return alignmentConfig{Source: "VRChatFeed", Reference: "VRChatAudio", Output: "VaOBSAlignedAudio", MaxDelayMS: 60000}
}

func TestConfigureCreatesAlignedOutputAndMutesOnlyMediaAudio(t *testing.T) {
	client, calls := mockOBS(t, baseReply)
	if err := configureAlignment(client, testConfig(), 42); err != nil {
		t.Fatal(err)
	}
	created, copiedTracks, copiedVolume := false, false, false
	for _, call := range calls() {
		switch call.RequestType {
		case "CreateInput":
			created = call.Data["inputName"] == "VaOBSAlignedAudio" && call.Data["inputKind"] == alignmentInputKind && call.Data["sceneName"] == "VRChat"
			settings := call.Data["inputSettings"].(map[string]any)
			if settings["reference_source"] != "VRChatAudio" || settings["feed_source"] != "VRChatFeed" || settings["stream_generation"] != float64(42) {
				t.Fatalf("wrong settings: %#v", settings)
			}
		case "SetInputAudioTracks":
			tracks := call.Data["inputAudioTracks"].(map[string]any)
			copiedTracks = call.Data["inputName"] == "VaOBSAlignedAudio" && tracks["2"] == true && tracks["1"] == false
		case "SetInputVolume":
			copiedVolume = call.Data["inputName"] == "VaOBSAlignedAudio" && call.Data["inputVolumeMul"] == .8
		}
		if strings.HasPrefix(call.RequestType, "Set") && call.Data["inputName"] == "VRChatAudio" {
			t.Fatalf("changed reference: %#v", call)
		}
		if call.RequestType == "SetInputSettings" && call.Data["inputName"] == "VRChatFeed" {
			t.Fatal("changed original media/video settings")
		}
	}
	history := calls()
	last := history[len(history)-1]
	if !created || !copiedTracks || !copiedVolume || last.RequestType != "SetInputMute" || last.Data["inputName"] != "VRChatFeed" || last.Data["inputMuted"] != true {
		t.Fatalf("incomplete output setup: %#v", history)
	}
}

func TestConfigureRequiresPlugin(t *testing.T) {
	client, calls := mockOBS(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "GetInputKindList" {
			return map[string]any{"inputKinds": []string{}}, 100
		}
		return baseReply(call)
	})
	err := configureAlignment(client, testConfig(), 1)
	if err == nil || !strings.Contains(err.Error(), "not loaded") || len(calls()) != 1 {
		t.Fatalf("missing plugin not handled: %v", err)
	}
}

func TestExistingOutputNameConflictDoesNotMutateOBS(t *testing.T) {
	client, calls := mockOBS(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "GetInputList" {
			return map[string]any{"inputs": []map[string]any{{"inputName": "VaOBSAlignedAudio", "inputKind": "ffmpeg_source"}}}, 100
		}
		return baseReply(call)
	})
	err := configureAlignment(client, testConfig(), 1)
	if err == nil || !strings.Contains(err.Error(), "different type") {
		t.Fatalf("expected conflict: %v", err)
	}
	for _, call := range calls() {
		if strings.HasPrefix(call.RequestType, "Set") || strings.HasPrefix(call.RequestType, "Create") {
			t.Fatalf("modified existing input: %#v", call)
		}
	}
}

func TestStreamChangeResetsOnlyAlignedOutput(t *testing.T) {
	client, calls := mockOBS(t, baseReply)
	if err := resetAlignment(client, "VaOBSAlignedAudio", 123); err != nil {
		t.Fatal(err)
	}
	call := calls()[0]
	if call.RequestType != "SetInputSettings" || call.Data["inputName"] != "VaOBSAlignedAudio" || call.Data["overlay"] != true || len(call.Data["inputSettings"].(map[string]any)) != 1 {
		t.Fatalf("wrong reset: %#v", call)
	}
}

func TestExistingAlignedOutputKeepsVolumeAndTrackChoices(t *testing.T) {
	client, calls := mockOBS(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "GetInputList" {
			return map[string]any{"inputs": []map[string]any{{"inputName": "VaOBSAlignedAudio", "inputKind": alignmentInputKind}}}, 100
		}
		if call.RequestType == "GetSceneItemList" {
			return map[string]any{"sceneItems": []map[string]any{{"sourceName": "VaOBSAlignedAudio", "sceneItemId": 42}}}, 100
		}
		return baseReply(call)
	})
	if err := configureAlignment(client, testConfig(), 9); err != nil {
		t.Fatal(err)
	}
	for _, call := range calls() {
		if call.RequestType == "CreateInput" || call.RequestType == "CreateSceneItem" || call.RequestType == "SetInputAudioTracks" || call.RequestType == "SetInputVolume" {
			t.Fatalf("replaced existing output controls: %#v", call)
		}
	}
}

func TestMissingReferenceStopsBeforeMutation(t *testing.T) {
	client, calls := mockOBS(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "GetInputSettings" {
			return nil, 600
		}
		return baseReply(call)
	})
	err := configureAlignment(client, testConfig(), 1)
	if err == nil || !strings.Contains(err.Error(), "VRChatAudio") {
		t.Fatalf("missing reference not reported: %v", err)
	}
	for _, call := range calls() {
		if strings.HasPrefix(call.RequestType, "Set") || strings.HasPrefix(call.RequestType, "Create") {
			t.Fatalf("modified OBS without reference: %#v", call)
		}
	}
}

func TestPartialOutputFailureRemovesNewInputAndLeavesFeedUnmuted(t *testing.T) {
	client, calls := mockOBS(t, func(call obsCall) (map[string]any, int) {
		if call.RequestType == "SetInputAudioTracks" {
			return nil, 400
		}
		return baseReply(call)
	})
	if err := configureAlignment(client, testConfig(), 1); err == nil {
		t.Fatal("expected output setup failure")
	}
	removed := false
	for _, call := range calls() {
		if call.RequestType == "RemoveInput" && call.Data["inputName"] == "VaOBSAlignedAudio" {
			removed = true
		}
		if call.RequestType == "SetInputMute" && call.Data["inputName"] == "VRChatFeed" {
			t.Fatal("muted original feed after failed output setup")
		}
	}
	if !removed {
		t.Fatal("left an incomplete output input behind")
	}
}

func TestAlignmentConfigurationValidation(t *testing.T) {
	for _, config := range []alignmentConfig{
		{Source: "VRChatAudio", Reference: "VRChatAudio", Output: "Aligned", MaxDelayMS: 60000},
		{Source: "VRChatFeed", Reference: "VRChatAudio", Output: "VRChatFeed", MaxDelayMS: 60000},
		{Source: "VRChatFeed", Reference: "VRChatAudio", Output: "Aligned", MaxDelayMS: 121000},
	} {
		if config.validate() == nil {
			t.Fatalf("invalid config accepted: %#v", config)
		}
	}
}
