# Main Menu Video Player

[![Build](https://github.com/SulfurNitride/Fallout4-Main-Menu-Video-Player/actions/workflows/build.yml/badge.svg)](https://github.com/SulfurNitride/Fallout4-Main-Menu-Video-Player/actions/workflows/build.yml)

Main Menu Video Player replaces Fallout 4's native main-menu movie with a
random ordinary video while keeping the Scaleform menu and console in front.
It supports video audio, loops the selected file, and avoids selecting the
same file twice in a row when more than one video is available.

Playback continues after alt-tab in borderless fullscreen. In windowed or
exclusive-fullscreen modes, playback pauses while Fallout is unfocused.

## Requirements and supported runtimes

- Fallout 4 Script Extender (F4SE) matching the installed game.
- One of these Fallout 4 executable versions:

| FOMOD choice | Supported Fallout 4 runtimes |
| --- | --- |
| OG | 1.10.163 |
| NG | 1.10.980 and 1.10.984 |
| AE | 1.11.137, 1.11.159, 1.11.169, 1.11.191, and 1.11.221 |

The plugin includes its runtime addresses and does not require Address
Library.

## Installation

1. Install the release ZIP with Mod Organizer 2 or another FOMOD-capable mod
   manager.
2. Select the option matching the version shown in your Fallout 4
   executable's properties.
3. Let this mod win conflicts for `Video/MainMenuLoop.bk2`.
4. Add one or more videos directly to `Data/MainMenuVideos`.
5. Start Fallout 4 through F4SE.

For a manual installation, copy everything under `common` to Fallout 4's
`Data` directory, then copy the matching runtime folder there as well.

The plugin enables Fallout's native main-menu Bink layer and disables the
vanilla main-menu music in memory before the menu initializes. It does not
rewrite the user's Fallout INIs. The included MO2 INI tweak is a fallback for
mod-manager configurations that apply it.

The included `MainMenuLoop.bk2` is a silent, black, 3840x2160, 60 fps,
five-second carrier. Fallout loops this carrier transparently while the
selected ordinary video continues. The plugin replaces its decoded pixels
with the selected video, leaving the menu controls above it.

Supported container extensions are:

`3g2`, `3gp`, `asf`, `avi`, `f4v`, `flv`, `m4v`, `mkv`, `mov`, `mp4`,
`mpeg`, `mpg`, `ogv`, `qt`, `vob`, `webm`, and `wmv`.

Videos are center-cropped to fill the native main-menu frame without
stretching. One video is selected randomly for each main-menu session and
loops until that session closes.

## Configuration

`Data/F4SE/Plugins/MainMenuVideoPlayer.ini` controls:

- `EnableNativeMainMenuBink`: force Fallout's native menu-video layer on.
- `MuteVanillaMenuMusic`: mute Fallout's original main-menu music without
  muting the selected video's audio.
- `KeepPlayingWhenBorderless`: continue video and audio after alt-tab in
  borderless fullscreen.

The log is written to
`Data/F4SE/Plugins/MainMenuVideoPlayer.log`.

## Troubleshooting

If the screen remains black, confirm that a supported video exists directly
inside `Data/MainMenuVideos` and inspect the log for the selected filename or
decoder error.

If the vanilla movie appears, make sure this mod wins the
`Video/MainMenuLoop.bk2` conflict.

If both the vanilla music and video audio play, leave
`MuteVanillaMenuMusic=1` in the plugin INI and confirm that the matching DLL
was selected in the FOMOD.

## Building

The supported build environment is Visual Studio 2022 on Windows. The
repository pins the vcpkg revision used for dependency resolution in
`.github/workflows/build.yml`; the workflow builds all three runtime DLLs,
assembles the FOMOD, validates its structure, and uploads the ZIP as an
artifact. A tag matching `v*` also publishes the ZIP to a GitHub release.

To reproduce the workflow locally, follow the configure and build commands in
the workflow using the same vcpkg commit and the
`x64-windows-static-md` triplet.

## License

Project source is available under the [MIT License](LICENSE). The distributed
DLL statically incorporates third-party libraries under their respective
licenses; see [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and
[`licenses`](licenses).
