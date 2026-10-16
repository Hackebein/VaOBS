# VaOBS

An audio-only OBS plugin that reads video URLs from VRChat logs, plays their audio,
and automatically aligns it with VRChat for recording.

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
