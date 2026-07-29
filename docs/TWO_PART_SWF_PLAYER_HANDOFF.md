# Two-part Pip-Boy SWF player handoff

## Purpose and current baseline

This document is the starting point for the next development conversation. The
goal is to replace the temporary two-option terminal picker with a two-part
Scaleform interface without replacing Fallout 4's `PipboyMenu.swf` or assuming
one fixed Pip-Boy model.

Version 0.1.1 is the working baseline:

- The holotape is added to the player's inventory by a start-game-enabled
  quest.
- Its vanilla terminal offers random Movie and TV playback.
- The native DLL scans `Data/MovieVideos` and `Data/TVVideos`, decodes ordinary
  video formats, uploads frames to the active Pip-Boy render target, and plays
  audio through XAudio.
- The native overlay has seek, previous, play/pause, next, and stop controls.
- The projected green cursor follows the full-screen Fallout cursor closely
  enough for current use. The flat `CursorMenu` cursor is hidden only while the
  native player owns the Pip-Boy surface and is restored on every exit path.
- Tab or Escape leaves playback.
- F4SE serialization already stores a stable media ID, playback position,
  paused state, and loop state.
- Main-menu playback, BK2 carrier switching, XWM sidecars, and the disabled
  world-screen stubs are separate from this work and must continue to function.

There is no production SWF in 0.1.1. Any tiny generated
`MMVPVideoPlayer.swf` found under an old build directory is a discarded marker,
not a player and not a package input.

## Required user experience

The final flow should be:

```text
Pip-Boy Inventory
  -> load Main Menu Video Player holotape
  -> MMVPBrowser.swf
      -> Movies
      -> TV
      -> Random
      -> Resume, when a saved item still exists
  -> MMVPPlayer.swf
      -> video behind responsive controls
      -> Back returns to MMVPBrowser.swf
  -> Back again returns to the normal Pip-Boy
```

The browser and player are separate programs loaded through the vanilla
holotape-program container. Neither SWF should force itself onto the whole
Pip-Boy menu or patch a base-game SWF. Loading and unloading them through the
normal program lifecycle is what prevents a blank or inescapable Pip-Boy.

## Compatibility contract

Custom Pip-Boys are supported when they preserve Fallout's vanilla holotape
program loader and expose a usable program stage. The implementation must not:

- replace `Interface/PipboyMenu.swf`;
- use absolute desktop coordinates as Pip-Boy coordinates;
- assume the vanilla 876x700 render-target size;
- assume the screen is rectangular, centered, or the same aspect ratio as the
  desktop;
- leave `CursorMenu`, input capture, audio, or render hooks in an altered state
  after either SWF unloads.

Both SWFs should lay themselves out from the actual Stage dimensions. Controls
need a configurable safe-area inset so curved or unusually cropped custom
screens remain usable. The DLL should continue discovering the real D3D render
target and report its dimensions instead of treating the INI values as the
authoritative size. The INI dimensions can remain a diagnostic fallback.

## Recommended render architecture

The current native player copies a completed frame over the entire Pip-Boy
render target. That works for the 0.1.1 native overlay, but it would also erase
an SWF drawn into the same target. A real two-part player therefore needs the
decoded frame to become an image inside Scaleform rather than a final
full-target overwrite.

The preferred design is:

1. Keep FFmpeg decoding, audio, timing, file scanning, shuffle bags, and
   serialization in the F4SE DLL.
2. Allocate/update a native D3D11 texture for the decoded frame.
3. expose that texture to Scaleform as an external image/resource;
4. let `MMVPPlayer.swf` place the image beneath its own controls;
5. size the image with contain or center-crop math inside the current Stage
   bounds.

This preserves crisp Scaleform controls, correct z-order, mouse hit testing,
and custom-Pip-Boy scaling. If direct external-texture registration proves
impossible in Fallout 4's Scaleform build, the fallback is to keep the SWF as
an input/lifecycle layer and have the DLL composite the UI into its native
output. That fallback is less desirable because it duplicates Scaleform
layout and accessibility work in C++.

Do not begin by embedding MP4 playback in Flash. Fallout 4's Scaleform runtime
does not provide a dependable modern media stack. The SWFs are the interface;
the DLL remains the media engine.

## File and build layout

Add source files without committing compiled SDKs or cache directories:

```text
interface/
  browser/
    MMVPBrowser.as
    MMVPBrowser.mxml
  player/
    MMVPPlayer.as
    MMVPPlayer.mxml
  shared/
    MMVPBridge.as
    MMVPLayout.as
    MMVPTypes.as
scripts/
  build-pipboy-swfs.sh
package/common/Interface/Programs/
  MMVPBrowser.swf
  MMVPPlayer.swf
```

Apache Flex 4.16.1 `mxmlc` is known to be available on the current development
machine at:

```text
/home/luke/.cache/mmvp-flex/apache-flex-sdk-4.16.1-bin/bin/mxmlc
```

The repository script must accept `FLEX_HOME` or find `mxmlc` on `PATH`; it
must not hardcode that machine-specific location. Build both SWFs for the
ActionScript/Flash version supported by Fallout 4's Scaleform runtime. Add an
offline validation script that checks the SWF header, expected exported
classes, and bridge symbol names. Only after the SWFs load successfully in
game should CMake and CI require them in the FOMOD.

## Native-to-Scaleform bridge

Use one stable bridge object under the active program root, for example
`root.f4se.plugins.MainMenuVideoPlayer`. Keep commands and state versioned so a
new DLL can reject an incompatible SWF cleanly.

Suggested SWF-to-native commands:

| Command | Arguments | Result |
| --- | --- | --- |
| `getApiVersion` | none | Native bridge version |
| `openBrowser` | none | Begin/refresh browser session |
| `listMedia` | channel, offset, limit, sort | One page plus total count |
| `playMedia` | stable media ID | Open player at saved position if available |
| `playRandom` | channel | Select through the existing shuffle bag |
| `resume` | none | Resume serialized item |
| `togglePause` | none | Pause/resume without creating a new session |
| `seek` | seconds or normalized position | Seek current decoder and audio |
| `previous` | none | Select prior session item when available |
| `next` | none | Select the next shuffled item |
| `stop` | none | Stop and return to browser |
| `setVolume` | normalized value | Update audio without recreating voices |
| `closeProgram` | none | Release all player ownership and return normally |

Suggested native-to-SWF state:

```text
apiVersion
sessionGeneration
screenWidth
screenHeight
channel
mediaId
displayName
durationSeconds
positionSeconds
buffering
paused
playing
errorCode
errorText
browserTotal
browserOffset
browserItems[]
```

Pass stable media IDs, not raw absolute paths. The current media library can
derive an ID from the channel-relative normalized path. A browser item should
contain ID, display name, channel, extension, and optional duration. Page the
list rather than pushing thousands of entries through Scaleform in one call.
A default page of 25–50 items is appropriate.

The bridge should send events or update a small state object on meaningful
changes. Do not call `UI.Set` every frame for playback time. A 4–10 Hz progress
update is enough for the seek bar; the SWF can interpolate visually between
updates while playing.

## Lifecycle and state machine

Treat browsing, playback, and menu visibility as explicit states:

```text
Inactive
  -> BrowserVisible
  -> PlayerOpening
  -> PlayerPlaying <-> PlayerPaused
  -> BrowserVisible
  -> Inactive
```

Rules:

- Loading the browser never starts random playback by itself.
- Selecting an item increments the session generation once and opens it.
- Pausing does not increment the generation, close the decoder, or discard the
  timestamp.
- A temporary Pip-Boy/menu hide pauses audio and decoding but preserves the
  session.
- Restoring the same player resumes from the serialized/current timestamp.
- Returning from player to browser stops presentation while retaining the
  resume record.
- Explicit Stop clears the active presentation but should keep or clear resume
  history according to one documented setting.
- Exiting the program releases input, restores the Fallout cursor, stops audio,
  unregisters the external image, and returns control to the vanilla Pip-Boy.
- Save/load restores the media ID and position only if the file still exists.
  A missing file returns to the browser with a readable error.
- Every error and every unexpected SWF unload must run the same idempotent
  cleanup path.

The current terminal commands call random activation directly, which is why
reloading a choice creates a new session instead of resuming. Refactor that
entry point into separate `OpenBrowser`, `PlayMedia`, `PlayRandom`, and
`Resume` operations before connecting the new SWFs.

## Input and cursor plan

Let Scaleform own hit testing while either SWF is visible. The C++ input router
should handle only global lifecycle keys and gamepad translation that cannot
be expressed through the program SWF.

- Use the program Stage mouse coordinates for SWF controls.
- Do not draw the current projected green C++ cursor on top of the SWF cursor.
- Hide Fallout's flat `CursorMenu` cursor only after the program cursor is
  confirmed visible.
- Restore it before unloading the player, including decode errors and game
  shutdown.
- Back from player opens browser; Back from browser closes the holotape
  program. A held key must not trigger both transitions.
- Support keyboard, mouse, and controller focus. Never require a pointer to
  leave playback.

The 0.1.1 projected-cursor implementation should remain available behind the
native-player path until the SWF player has passed the complete test matrix.

## Implementation sequence

1. Add a native browser model that returns paged, sorted Movie/TV items and
   stable media IDs. Unit-test path normalization and pagination.
2. Split the current random terminal activation into browser, selected,
   random, and resume commands without changing rendering.
3. Implement `MMVPBrowser.swf` using only text/list controls and prove that
   loading, selecting, Back, repeated opening, and custom-Pip-Boy layout work.
4. Implement the versioned bridge and event/state transport.
5. Prototype native D3D11 texture registration as a Scaleform image. Prove one
   static test frame appears behind an SWF button before connecting FFmpeg.
6. Implement `MMVPPlayer.swf`, responsive layout, progress updates, seeking,
   pause, previous/next, stop, and return-to-browser.
7. Connect decoder output and make audio/menu visibility follow the explicit
   state machine.
8. Migrate serialization by version while retaining compatibility with 0.1.1
   save data.
9. Remove the temporary terminal options only after both SWFs are reliable.
   Keep a recovery terminal entry or INI option for one release.
10. Add the SWF build and validation steps to CMake/CI and package both SWFs.

## Likely native files to change

- `src/PipBoyPlayer.cpp/.h`: split browser/session commands, lifecycle, external
  image presentation, and state reporting.
- `src/MediaLibrary.cpp/.h`: stable IDs, metadata, sorting, paging, and lookup.
- `src/InputRouter.cpp/.h`: browser/player Back behavior and controller input.
- `src/Serialization.cpp/.h`: versioned resume record and migration.
- `src/F4SEMinimal.h`: only the smallest verified Scaleform definitions needed
  for the external image and bridge.
- `src/Plugin.cpp`: register bridge callbacks and cleanup messaging.
- `CMakeLists.txt`, `.github/workflows/build.yml`, and FOMOD metadata: build,
  validate, and package the two SWFs after the in-game prototype works.
- `tools/WorldPluginGenerator/Program.cs` and the terminal fragment: point the
  holotape at the browser program while retaining a recovery path.

## Required validation matrix

Build and inspect all three DLL variants on every change:

| Variant | Runtime |
| --- | --- |
| OG | 1.10.163 |
| NG | 1.10.980 and 1.10.984 |
| AE | 1.11.137 through 1.11.221 addresses currently supported by the project |

In-game tests should cover:

- vanilla Pip-Boy and at least two structurally different custom Pip-Boys;
- 16:9, ultrawide, and 4K desktop resolutions;
- mouse, keyboard-only, and controller-only operation;
- empty folders, one item, hundreds of items, nested folders, duplicate display
  names, removed files, and Unicode filenames;
- MP4/WebM/other FFmpeg formats, mono and stereo audio, long videos, seek near
  the end, and corrupt inputs;
- pause/resume, Pip-Boy hide/show, player-to-browser Back, browser-to-Pip-Boy
  Back, repeated open/close, save/load while paused, and game shutdown;
- main-menu ordinary video, BK2 selection, XWM sidecar, and all existing
  hotkeys after the Pip-Boy changes;
- `EnableWorldScreens=0` to confirm the television/projector stubs remain
  inactive.

## Completion criteria

The SWF work is complete only when a user can browse individual Movie and TV
files, start one, pause it, leave and resume at the same position, return to
the browser, and exit to a fully functional Pip-Boy without a blank screen,
stuck input, duplicate cursor, audio leak, or dependence on the vanilla
Pip-Boy's exact geometry. OG, NG, and AE packages must all still build, and the
main-menu player must pass its existing regression checks.
