# VaOBS

An OBS plugin that captures videos playing in VRChat and delays their clean
audio to match VRChat during recording.

## Setup

1. Use Windows x64 (tested with OBS 32.2.2). Extract the ZIP, close OBS, run
   `install-plugin.ps1` in PowerShell (as administrator if needed), then start OBS.
2. Add a Media Source, such as `VRChatFeed`, on recording track 2 and VRChat
   application audio capture, such as `VRChatAudio`, on track 1.
3. Add a **VaOBS** source. In its Properties, select those two sources.

VaOBS reads the local VRChat logs and updates the media source. It mutes the
original media audio and sends the delayed audio through the VaOBS source on
the same recording tracks. OBS saves the settings. Remove the VaOBS source to
restore the media source's previous mute state.

Use **Refresh status** in Properties to check alignment. A 21-second delay
needs about 27 seconds of shared audio to calibrate; output stays silent until
it matches. For other scenes, add the existing VaOBS source and keep the media
source active.

To uninstall, remove the VaOBS source, close OBS, and delete
`%PROGRAMDATA%\obs-studio\plugins\vaobs-audio-align`.

## Build

With Git and PowerShell on Windows, run:

```powershell
.\scripts\build-windows.ps1
```

The script downloads the pinned C++ toolchain and OBS headers, runs unit tests,
and builds the ZIP in `dist`. Add `-ObsBinPath` with your OBS `bin\64bit`
directory to also test playback and alignment against real libobs.

## License

[GPL-2.0-or-later](LICENSE). Original and third-party notices are retained.

## Support

[Report an issue](https://github.com/Hackebein/VaOBS/issues/new) ·
[Hackebein's Research Lab](https://discord.gg/AqCwGqqQmW)
