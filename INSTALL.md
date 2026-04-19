# Installing Giannitune

**End users**: grab the installer for your platform from
[Gumroad](https://jonnygucci1.gumroad.com/l/giannitune). The installer
drops the plugin into the correct system folder and you're done.

This file is for developers who built the plugin from source and want
to install the resulting `.vst3` / `.component` manually.

## Windows (manual install)

Copy the built bundle:

```sh
xcopy /E /I /Y \
    build\Giannitune_artefacts\Release\VST3\Giannitune.vst3 \
    "C:\Program Files\Common Files\VST3\Giannitune.vst3"
```

Rescan VST3 plugins in your DAW.

## macOS (manual install)

```sh
sudo rm -rf /Library/Audio/Plug-Ins/VST3/Giannitune.vst3
sudo rm -rf /Library/Audio/Plug-Ins/Components/Giannitune.component
sudo cp -R build/Giannitune_artefacts/Release/VST3/Giannitune.vst3 \
    /Library/Audio/Plug-Ins/VST3/
sudo cp -R build/Giannitune_artefacts/Release/AU/Giannitune.component \
    /Library/Audio/Plug-Ins/Components/
sudo xattr -rc /Library/Audio/Plug-Ins/VST3/Giannitune.vst3
sudo xattr -rc /Library/Audio/Plug-Ins/Components/Giannitune.component
```

Logic Pro caches plugin scans aggressively; after install run:

```sh
rm -rf ~/Library/Caches/AudioUnitCache
killall -9 AudioComponentRegistrar 2>/dev/null || true
```

Then restart Logic.

## Tested DAWs

- Reaper (Windows + macOS)
- Ableton Live 11+
- FL Studio 21+
- Logic Pro 10.7+ (macOS AU)
- Cubase 12+
- Studio One 6+

## Uninstall

Remove the files from the install locations above.

Preferences (key/scale defaults) live at:

- Windows: `%APPDATA%\Giannitune\`
- macOS: `~/Library/Application Support/Giannitune/`
