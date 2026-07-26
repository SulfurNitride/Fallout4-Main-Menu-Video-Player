# Main Menu Video Player

Main Menu Video Player replaces Fallout 4's native main-menu movie with a
random ordinary video while keeping the Scaleform menu and console in front.
It supports video audio, loops the selected file, and avoids selecting the
same file twice in a row when more than one video is available.

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
vanilla main-menu music in memory before the menu initializes.

The included `MainMenuLoop.bk2` is a silent, black, 3840x2160, 60 fps,
five-second video. Fallout loops this video transparently while the
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
