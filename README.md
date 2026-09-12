# Main Menu Video Player

Main Menu Video Player lets Fallout 4 play your own videos on the main menu. Put your videos in the included folders, start the game through F4SE, and the mod handles the rest.

## Requirements

- [Fallout 4 Script Extender (F4SE)](https://f4se.silverlock.org/)

The mod does not require or include an ESP/ESL plugin. Fallout 4 `1.11.240` requires F4SE `0.7.9`.

Choose the FOMOD option matching your game:

| Option | Fallout 4 version |
| --- | --- |
| OG | 1.10.163 |
| NG | 1.10.980 or 1.10.984 |
| AE | 1.11.137, 1.11.159, 1.11.169, 1.11.191, 1.11.221, or 1.11.240 |

Address Library is not required.

## Installation

If upgrading from an experimental 0.1.x installation, uninstall the old package and disable `MMVP_WorldScreens.esp` first. An overwrite install cannot remove obsolete ESP, Pip-Boy, or world-screen files.

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

Both folders support subfolders.

Common formats such as MP4, MKV, AVI, MOV, WebM, WMV, and MPEG are supported. The main-menu folder also supports native Fallout 4 BK2 videos.

MMVP detects Fallout's window aspect ratio and preserves it while composing video into the native main-menu carrier. This supports 21:9, 32:9, 16:10, and other non-16:9 resolutions without prematurely cropping everything to 16:9. Set `MatchWindowAspect=0` in the INI if another menu mod already performs its own aspect correction.

MMVP uses auto-threaded Bicubic scaling by default when a video does not match the presentation dimensions. Set `ScalingAlgorithm=Spline36` in the INI to opt into higher-quality resizing through FFmpeg's zscale filter. Spline36 has higher temporary CPU and system-memory requirements; all of its scaling resources are released when the main menu closes. Videos fill the detected presentation frame, so only source content outside that final aspect ratio is center-cropped.

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

Set `HelpMilliseconds=0` in that file to disable the upper-left filename and controls card completely.

## BK2 audio

Audio tracks embedded in a selected BK2 obey `MainMenuVolume` and the Page Up and Page Down volume controls. MMVP asks Bink for every selected file's actual track IDs and applies Fallout's main-menu source volume as a multiplier; silent BK2 files remain silent.

If a BK2 video has no built-in audio, place an XWM file beside it with the same name:

```text
AwesomeVideo.bk2
AwesomeVideo.xwm
```

A matching XWM is an explicit audio override. MMVP keeps the BK2 audio clock running but sets every embedded track to zero volume, then plays only the XWM soundtrack. This same-name sidecar behavior applies when no dedicated soundtrack is playing.

## Dedicated main-menu audio

Put audio-capable files in `Data/MainMenuAudio`; MMVP detects the library automatically. It accepts standalone audio such as MP3, FLAC, OGG, Opus, WAV, WMA, XMA, and XWM, plus video containers such as MP4, MKV, WebM, AVI, MOV, and BK2. It ignores the video stream and plays the first decodable audio stream.

An audio file whose basename matches the selected video is treated as its soundtrack and replaces embedded audio. If no same-name soundtrack exists, MMVP keeps decodable embedded audio; for a silent video it selects a random file from `MainMenuAudio` and avoids an immediate repeat. Press N for another random soundtrack. Press M to switch between dedicated and original video audio when both are available.

## Configuration and logs

Settings are in `Data/F4SE/Plugins/MainMenuVideoPlayer.ini`.

The log is written to `Data/F4SE/Plugins/MainMenuVideoPlayer.log`.

## Building from source

The GitHub Actions build is self-contained on `windows-2022`. The optional Linux-hosted cross-build in `scripts/build.sh` uses `Containerfile`, based on the public `highcanfly/llvm4msvc` 0.9.3 amd64 manifest pinned by digest. The container also checks out the pinned vcpkg revision and installs the FFmpeg build tools. Override `MMVP_BASE_IMAGE` when using an equivalent local image, and optionally choose a different output image with `MMVP_BUILDER_IMAGE`.

## Credits

Made with help from Codex.
