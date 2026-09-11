# Main Menu Video Player

Main Menu Video Player lets Fallout 4 play your own videos on the main menu. Put your videos in the included folders, start the game through F4SE, and the mod handles the rest.

## Requirements

- Fallout 4 Script Extender (F4SE)

Choose the FOMOD option matching your game:

| Option | Fallout 4 version |
| --- | --- |
| OG | 1.10.163 |
| NG | 1.10.980 or 1.10.984 |
| AE | 1.11.137, 1.11.159, 1.11.169, 1.11.191, or 1.11.221 1.11.240 |

Address Library is not required.

## Installation

1. Install the FOMOD with Mod Organizer 2 or another compatible mod manager.
2. Choose the option matching your `Fallout4.exe` version.
3. Let this mod win any conflict for `Video/MainMenuLoop.bk2`.
4. Add your videos to the folders listed below.
5. Launch Fallout 4 through F4SE.

## Video folders

| Folder | Used for |
| --- | --- |
| `Data/MainMenuVideos` | Main-menu videos |
| `Data/MainMenuAudio` | Optional independent main-menu soundtracks |

Common formats such as MP4, MKV, AVI, MOV, WebM, WMV, and MPEG are supported. The main-menu folder also supports native Fallout 4 BK2 videos.

## Main-menu controls

| Key | Action |
| --- | --- |
| Tab | Play another random video |
| Backspace | Stop the video |
| Page Up | Increase video volume |
| Page Down | Decrease video volume |
| N | Play another dedicated soundtrack |
| M | Toggle dedicated/original video audio |

The hotkeys can be changed or disabled in `Data/F4SE/Plugins/MainMenuVideoPlayer.ini`.

## BK2 audio

Audio tracks embedded in a selected BK2 obey `MainMenuVolume` and the Page Up
and Page Down volume controls. MMVP asks Bink for every selected file's actual
track IDs and applies Fallout's main-menu source volume as a multiplier; silent
BK2 files remain silent.

If a BK2 video has no built-in audio, place an XWM file beside it with the same name:

```text
AwesomeVideo.bk2
AwesomeVideo.xwm
```

A matching XWM is an explicit audio override. MMVP keeps the BK2 audio clock
running but sets every embedded track to zero volume, then plays only the XWM
soundtrack. This same-name sidecar behavior applies when no dedicated
soundtrack is playing.

## Dedicated main-menu audio

Put audio-capable files in `Data/MainMenuAudio`; MMVP detects the library
automatically. It accepts standalone audio such as MP3, FLAC, OGG,
Opus, WAV, WMA, XMA, and XWM, plus video containers such as MP4, MKV, WebM,
AVI, MOV, and BK2. It ignores the video stream and plays the first decodable
audio stream.

An audio file whose basename matches the selected video is treated as its
soundtrack and replaces embedded audio. If no same-name soundtrack exists,
MMVP keeps decodable embedded audio; for a silent video it selects a random
file from `MainMenuAudio` and avoids an immediate repeat. Press N for another
random soundtrack. Press M to switch between dedicated and original video
audio when both are available.

## Configuration and logs

Settings are in `Data/F4SE/Plugins/MainMenuVideoPlayer.ini`.

The log is written to `Data/F4SE/Plugins/MainMenuVideoPlayer.log`.

## Credits

Made with help from Codex.
