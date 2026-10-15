// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"fmt"
	"strings"

	"github.com/andreykaipov/goobs"
	"github.com/andreykaipov/goobs/api/requests/inputs"
	"github.com/andreykaipov/goobs/api/requests/sceneitems"
)

const alignmentInputKind = "vaobs_auto_align_source"

type alignmentConfig struct {
	Source, Reference, Output string
	MaxDelayMS                int
	InitialMuted              *bool
}

func (c alignmentConfig) validate() error {
	if strings.TrimSpace(c.Source) == "" || strings.TrimSpace(c.Reference) == "" || strings.TrimSpace(c.Output) == "" {
		return fmt.Errorf("automatic alignment requires media, reference, and aligned output input names")
	}
	if c.Source == c.Reference || c.Output == c.Source || c.Output == c.Reference {
		return fmt.Errorf("media, timing reference, and aligned output must be different sources")
	}
	if c.MaxDelayMS < 1000 || c.MaxDelayMS > 120000 {
		return fmt.Errorf("maximum audio delay must be between 1 and 120 seconds")
	}
	return nil
}

func (c alignmentConfig) settings(generation int64) map[string]any {
	return map[string]any{
		"reference_source":  c.Reference,
		"feed_source":       c.Source,
		"max_delay_ms":      c.MaxDelayMS,
		"stream_generation": generation,
	}
}

// The new audio input renders on its own clock, so buffered sound continues
// even when the original media input stops delivering packets. Only the
// original media audio is muted; its video and the reference are retained.
func configureAlignment(client *goobs.Client, config alignmentConfig, generation int64) (err error) {
	if err = config.validate(); err != nil {
		return err
	}
	kinds, err := client.Inputs.GetInputKindList()
	if err != nil {
		return fmt.Errorf("query OBS input types: %w", err)
	}
	found := false
	for _, kind := range kinds.InputKinds {
		if kind == alignmentInputKind {
			found = true
			break
		}
	}
	if !found {
		return fmt.Errorf("VaOBS audio alignment plugin is not loaded; install vaobs-audio-align.dll and restart OBS")
	}
	for _, name := range []string{config.Reference, config.Source} {
		settings, check := client.Inputs.GetInputSettings(inputs.NewGetInputSettingsParams().WithInputName(name))
		if check != nil {
			return fmt.Errorf("audio input %q is unavailable: %w", name, check)
		}
		if settings.InputKind == alignmentInputKind {
			return fmt.Errorf("input %q cannot use another aligned output as its signal", name)
		}
	}
	list, err := client.Inputs.GetInputList()
	if err != nil {
		return err
	}
	exists := false
	for _, input := range list.Inputs {
		if input.InputName == config.Output {
			if input.InputKind != alignmentInputKind {
				return fmt.Errorf("input %q already exists with a different type", config.Output)
			}
			exists = true
		}
	}
	scene, err := client.Scenes.GetCurrentProgramScene()
	if err != nil {
		return fmt.Errorf("get the active OBS scene: %w", err)
	}
	sceneName := scene.CurrentProgramSceneName
	if sceneName == "" {
		sceneName = scene.SceneName
	}
	if sceneName == "" {
		return fmt.Errorf("OBS has no active program scene")
	}
	items, err := client.SceneItems.GetSceneItemList(sceneitems.NewGetSceneItemListParams().WithSceneName(sceneName))
	if err != nil {
		return err
	}
	inScene := false
	for _, item := range items.SceneItems {
		if item.SourceName == config.Output {
			inScene = true
		}
	}
	created := false
	defer func() {
		if err != nil && created {
			// A partially configured new input must not duplicate the original
			// feed. The feed is only muted after all output setup succeeds.
			if _, cleanup := client.Inputs.RemoveInput(inputs.NewRemoveInputParams().WithInputName(config.Output)); cleanup != nil {
				err = fmt.Errorf("%w; could not remove incomplete output: %v", err, cleanup)
			}
		}
	}()
	if !exists {
		tracks, check := client.Inputs.GetInputAudioTracks(inputs.NewGetInputAudioTracksParams().WithInputName(config.Source))
		if check != nil {
			return check
		}
		volume, check := client.Inputs.GetInputVolume(inputs.NewGetInputVolumeParams().WithInputName(config.Source))
		if check != nil {
			return check
		}
		mute, check := client.Inputs.GetInputMute(inputs.NewGetInputMuteParams().WithInputName(config.Source))
		if check != nil {
			return check
		}
		if config.InitialMuted != nil {
			mute.InputMuted = *config.InitialMuted
		}
		_, err = client.Inputs.CreateInput(inputs.NewCreateInputParams().WithSceneName(sceneName).
			WithInputName(config.Output).WithInputKind(alignmentInputKind).
			WithInputSettings(config.settings(generation)).WithSceneItemEnabled(true))
		if err != nil {
			return fmt.Errorf("create aligned audio output: %w", err)
		}
		created = true
		if _, err = client.Inputs.SetInputAudioTracks(inputs.NewSetInputAudioTracksParams().WithInputName(config.Output).
			WithInputAudioTracks(tracks.InputAudioTracks)); err != nil {
			return err
		}
		if _, err = client.Inputs.SetInputVolume(inputs.NewSetInputVolumeParams().WithInputName(config.Output).
			WithInputVolumeMul(volume.InputVolumeMul)); err != nil {
			return err
		}
		if _, err = client.Inputs.SetInputMute(inputs.NewSetInputMuteParams().WithInputName(config.Output).
			WithInputMuted(mute.InputMuted)); err != nil {
			return err
		}
	} else {
		if !inScene {
			if _, err = client.SceneItems.CreateSceneItem(sceneitems.NewCreateSceneItemParams().WithSceneName(sceneName).
				WithSourceName(config.Output).WithSceneItemEnabled(true)); err != nil {
				return err
			}
		}
		if _, err = client.Inputs.SetInputSettings(inputs.NewSetInputSettingsParams().WithInputName(config.Output).
			WithInputSettings(config.settings(generation)).WithOverlay(true)); err != nil {
			return err
		}
	}
	_, err = client.Inputs.SetInputMute(inputs.NewSetInputMuteParams().WithInputName(config.Source).WithInputMuted(true))
	if err != nil {
		return fmt.Errorf("mute original media audio to prevent an undelayed duplicate: %w", err)
	}
	return nil
}

func resetAlignment(client *goobs.Client, output string, generation int64) error {
	_, err := client.Inputs.SetInputSettings(inputs.NewSetInputSettingsParams().WithInputName(output).
		WithInputSettings(map[string]any{"stream_generation": generation}).WithOverlay(true))
	return err
}
