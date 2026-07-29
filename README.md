# Main Menu Video Player

Main Menu Video Player lets Fallout 4 play your own videos on the main menu and Pip-Boy. Put your videos in the included folders, start the game through F4SE, and the mod handles the rest.

The TV and projector workshop objects are included as disabled previews for future versions.

## Requirements

- Fallout 4 Script Extender (F4SE)
- `MMVP_WorldScreens.esp` enabled in your load order

Choose the FOMOD option matching your game:

| Option | Fallout 4 version |
| --- | --- |
| OG | 1.10.163 |
| NG | 1.10.980 or 1.10.984 |
| AE | 1.11.137, 1.11.159, 1.11.169, 1.11.191, or 1.11.221 |

Address Library is not required.

## Installation

1. Install the FOMOD with Mod Organizer 2 or another compatible mod manager.
2. Choose the option matching your `Fallout4.exe` version.
3. Enable `MMVP_WorldScreens.esp`.
4. Let this mod win any conflict for `Video/MainMenuLoop.bk2`.
5. Add your videos to the folders listed below.
6. Launch Fallout 4 through F4SE.

## Video folders

| Folder | Used for |
| --- | --- |
| `Data/MainMenuVideos` | Main-menu videos |
| `Data/MovieVideos` | Pip-Boy movies |
| `Data/TVVideos` | Pip-Boy shows |

Subfolders are supported.

Common formats such as MP4, MKV, AVI, MOV, WebM, WMV, and MPEG are supported. The main-menu folder also supports native Fallout 4 BK2 videos.

## Pip-Boy player

The **Main Menu Video Player** holotape is automatically added to your inventory. Find it under **Inventory > Misc**, load it, and choose a random movie or TV show.

Move the mouse to show the playback controls. You can seek, pause, play the previous or next video, and stop playback. Press Tab or Escape to leave the player.

## Main-menu controls

| Key | Action |
| --- | --- |
| Tab | Play another random video |
| Backspace | Stop the video |
| Page Up | Increase video volume |
| Page Down | Decrease video volume |

The hotkeys can be changed or disabled in `Data/F4SE/Plugins/MainMenuVideoPlayer.ini`.

## BK2 audio

If a BK2 video has no built-in audio, place an XWM file beside it with the same name:

```text
AwesomeVideo.bk2
AwesomeVideo.xwm
```

Do not add an XWM sidecar when the BK2 already has audio unless you intentionally want both tracks to play.

## TV and projector previews

The ESP includes workshop records and assets for two televisions, a projector, and a movie screen. They are development previews and are disabled by default. Keep `EnableWorldScreens=0` in the INI for this release.

## Configuration and logs

Settings are in `Data/F4SE/Plugins/MainMenuVideoPlayer.ini`.

The log is written to `Data/F4SE/Plugins/MainMenuVideoPlayer.log`.

## Credits

The Pip-Boy render-target work was informed by [Project Holo-Wind](https://github.com/rpgking117/Holo-Wind-Windows). The world assets can be regenerated with [Rust-BSA-BA2-Handler](https://github.com/SulfurNitride/Rust-BSA-BA2-Handler).

Made with help from Codex.
