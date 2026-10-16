# VaOBS

An audio-only OBS plugin that reads video URLs from VRChat logs, plays their audio,
and automatically aligns it with VRChat for recording.

After initial alignment, VaOBS follows timing drift with gradual, pitch-preserving
tempo correction. It holds the last trusted timing during silence or temporary
matching loss. Confirmed large timing jumps use a short crossfade. Hover over the
dock status or open the source properties to see playback speed and the remaining
timing correction.

## Setup

Requires Windows x64 and OBS Studio (tested with OBS 32.2.2).

1. Close OBS and extract the release ZIP.
2. Copy the `vaobs-audio-align` folder from the ZIP's `obs-plugin` folder into
   `%PROGRAMDATA%\obs-studio\plugins\`. Create the `plugins` folder if needed.
3. Start OBS and add VRChat application audio/window capture.
4. Add a **VaOBS** source and select that capture as its **Timing reference**.
5. Open **Docks → VaOBS** to see the status and delay.

## Build
```powershell
.\scripts\build-windows.ps1
```

## License

[GPL-2.0-or-later](LICENSE)

## Support

[Report an issue](https://github.com/Hackebein/VaOBS/issues/new)
[Hackebein's Research Lab](https://discord.gg/AqCwGqqQmW)
