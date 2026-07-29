# Shared two-part SWF browser/player handoff

## Purpose and current baseline

This document is the starting point for the next development conversation. The
goal is to replace the temporary two-option terminal picker with one shared
two-part Scaleform interface. The same browser/player pair must work from the
Pip-Boy and as the interaction menu for craftable televisions, projectors, and
video terminals. It must do this without replacing Fallout 4's
`PipboyMenu.swf` or assuming one fixed Pip-Boy or world-screen model.

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
- Main-menu playback, BK2 carrier switching, and XWM sidecars are separate from
  this work and must continue to function.
- Craftable TV/projector records and native world-texture code exist as disabled
  development stubs. Version 0.1.1 does not expose their interaction menu.

There is no production SWF in 0.1.1. Any tiny generated
`MMVPVideoPlayer.swf` found under an old build directory is a discarded marker,
not a player and not a package input.

## Required user experience

The Pip-Boy flow should be:

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

The world-object flow should use the same pair:

```text
Activate a placed MMVP object
  -> open MMVPBrowser.swf with that object as the output target
      -> Movies
      -> TV
      -> Random
      -> Resume this screen
      -> Choose/Pair Screen, when the object is a projector or terminal
  -> MMVPPlayer.swf
      -> controls the video playing on the selected world output
      -> Back returns to MMVPBrowser.swf
  -> Back again closes the menu and returns to the game
```

The SWFs are not separate Pip-Boy-only and workshop-only interfaces. They are
one responsive UI with a native launch context. The context tells them whether
they are controlling the Pip-Boy, a television, a projector and paired movie
screen, or a video terminal. Labels and available actions can change from that
context while browsing, playback controls, and visual style stay consistent.

## Surface context and world interaction

Every browser session must begin with an explicit output target:

```text
targetId
targetKind = PipBoy | Television | Projector | MovieScreen | Terminal
targetName
targetReferenceId
controllerReferenceId
pairedScreenReferenceId
supportsLocalVideo
supportsAudio
supportsPairing
```

Use stable game/reference identifiers in native code and serialization. Do not
pass raw pointers or absolute texture addresses into ActionScript.

Expected behavior by object:

- **Pip-Boy:** The program and video both appear on the Pip-Boy screen.
- **Television:** Activating the television opens the shared menu and makes that
  placed television the playback target.
- **Projector:** Activating the projector opens the shared menu. Playback goes
  to its paired movie screen. If no screen is paired, the browser first offers
  nearby compatible screens and clearly reports that pairing is required.
- **Movie screen:** Activating a screen may open the menu for its paired
  projector or offer compatible nearby projectors when unpaired.
- **Video terminal:** Activating the craftable MMVP terminal opens the shared
  menu. It can control its currently selected TV/projector/screen, offer a
  target picker, and optionally use its own display if its model has a suitable
  video surface.

Pairing should be explicit and saved. A nearest-screen search is useful for the
initial suggestion, but playback must not silently jump to a different screen
when workshop objects are moved. Store projector-to-screen and
terminal-to-output relationships by placed reference ID, validate them on
load, and ask the user to repair a missing pairing.

The custom menu is only a controller for world playback. Closing it must not
automatically stop a television or projector unless the user chooses Stop.
World audio should be spatial and attached to the output/controller instead of
using the Pip-Boy's non-spatial audio path.

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

The same rules apply when the pair is loaded as a world-object interaction
menu. The native plugin should register a proper custom-menu host and load the
same SWF assets with a world target context. It must not replace a vanilla
workshop, terminal, or activation menu.

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
full-target overwrite. World outputs separately need decoded frames routed to
the material/texture bridge belonging to the selected placed object.

The preferred design is:

1. Keep FFmpeg decoding, audio, timing, file scanning, shuffle bags, and
   serialization in the F4SE DLL.
2. Allocate/update a native D3D11 texture for each active playback session.
3. For Pip-Boy playback, expose that texture to Scaleform as an external
   image/resource.
4. For world playback, bind the texture to the selected TV or paired projector
   screen through `WorldTextureBridge`.
5. Let `MMVPPlayer.swf` show the local video image when the host supports it,
   or show a now-playing preview/status panel while it controls a world output.
6. Size the UI with contain or center-crop math inside the current Stage bounds.

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
  build-interface-swfs.sh
package/common/Interface/Programs/
  MMVPBrowser.swf
  MMVPPlayer.swf
```

Compile one browser SWF and one player SWF. Do not fork their ActionScript into
Pip-Boy and workshop copies. Native code may use two thin host paths—a vanilla
holotape-program host and a registered world custom-menu host—but both hosts
must load the same compiled browser/player assets and bridge API.

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
| `openBrowser` | target context | Begin/refresh a browser session for one output |
| `listTargets` | target kinds, offset, limit | List compatible placed outputs |
| `selectTarget` | stable target ID | Make a TV/screen/projector the controlled output |
| `pairTarget` | controller ID, output ID | Save a projector/screen or terminal/output pair |
| `clearPairing` | controller ID | Remove a saved world-object pairing |
| `listMedia` | channel, offset, limit, sort | One page plus total count |
| `playMedia` | target ID, stable media ID | Open on the selected output at its saved position |
| `playRandom` | channel | Select through the existing shuffle bag |
| `resume` | target ID | Resume the serialized item for that output |
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
targetId
targetKind
targetName
targetOnline
pairedTargetId
pairingRequired
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
  -> ResolveTarget
  -> PairingRequired, when needed
  -> BrowserVisible
  -> PlayerOpening
  -> PlayerPlaying <-> PlayerPaused
  -> BrowserVisible
  -> Inactive
```

Rules:

- Loading the browser never starts random playback by itself.
- A launch must resolve and validate its target before media can start.
- Pip-Boy resume state is separate from every placed world output's resume
  state.
- Selecting an item increments the session generation once and opens it.
- Pausing does not increment the generation, close the decoder, or discard the
  timestamp.
- A temporary Pip-Boy/menu hide pauses audio and decoding but preserves the
  session.
- Restoring the same player resumes from the serialized/current timestamp.
- Returning from player to browser hides the controller/player view while
  retaining the resume record. Pip-Boy presentation may pause; world playback
  may keep running.
- Explicit Stop clears the active presentation but should keep or clear resume
  history according to one documented setting.
- Exiting the program releases input, restores the Fallout cursor, unregisters
  menu-only resources, and returns control to the Pip-Boy or game. It stops
  Pip-Boy audio, but does not stop an independently running world output.
- Save/load restores target pairings plus each active output's media ID,
  position, and playback state only if both the reference and file still exist.
  Missing files or objects return a readable error.
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
- Back from player opens browser; Back from browser closes the holotape program
  or world interaction menu. A held key must not trigger both transitions.
- Support keyboard, mouse, and controller focus. Never require a pointer to
  leave playback.

The 0.1.1 projected-cursor implementation should remain available behind the
native-player path until the SWF player has passed the complete test matrix.

## Implementation sequence

1. Add a native browser model that returns paged, sorted Movie/TV items and
   stable media IDs. Unit-test path normalization and pagination.
2. Add a target registry for the Pip-Boy and placed televisions, projectors,
   screens, and terminals. Define pairing, capability, and save-data records.
3. Split the current random terminal activation into browser, selected,
   random, and resume commands without changing rendering.
4. Implement `MMVPBrowser.swf` using only text/list controls and prove that
   loading, selecting, Back, repeated opening, and custom-Pip-Boy layout work.
5. Register the world custom-menu host and open the same browser SWF when an
   MMVP workshop object is activated.
6. Implement target selection and explicit projector/screen plus
   terminal/output pairing before enabling video playback.
7. Implement the versioned bridge and event/state transport.
8. Prototype native D3D11 texture registration as a Scaleform image. Prove one
   static test frame appears behind an SWF button before connecting FFmpeg.
9. Implement `MMVPPlayer.swf`, responsive layout, progress updates, seeking,
   pause, previous/next, stop, and return-to-browser.
10. Route decoded frames either to the Pip-Boy external image or the selected
    world material, and use spatial XAudio for world outputs.
11. Migrate serialization by version while retaining compatibility with 0.1.1
    save data and storing world reference pairings/playback independently.
12. Remove the temporary terminal options only after both SWFs are reliable.
    Keep a recovery terminal entry or INI option for one release.
13. Add the SWF build and validation steps to CMake/CI and package both SWFs.

## Likely native files to change

- `src/PipBoyPlayer.cpp/.h`: split browser/session commands, lifecycle, external
  image presentation, and state reporting.
- `src/WorldPlayback.cpp/.h`: placed-output sessions, spatial audio ownership,
  target capability checks, and independent pause/resume state.
- `src/WorldTextureBridge.cpp/.h`: bind decoded textures to the selected
  television or movie-screen material.
- `src/MediaLibrary.cpp/.h`: stable IDs, metadata, sorting, paging, and lookup.
- `src/InputRouter.cpp/.h`: browser/player Back behavior and controller input.
- `src/Serialization.cpp/.h`: versioned resume records, placed-reference
  pairings, independent world playback, and migration.
- `src/F4SEMinimal.h`: only the smallest verified Scaleform definitions needed
  for the external image and bridge.
- `src/Plugin.cpp`: register bridge callbacks and cleanup messaging.
- `CMakeLists.txt`, `.github/workflows/build.yml`, and FOMOD metadata: build,
  validate, and package the two SWFs after the in-game prototype works.
- `tools/WorldPluginGenerator/Program.cs`: attach activation/controller data to
  the TV, projector, movie screen, and a new craftable MMVP video terminal.
- Papyrus sources: add a small world-screen controller script and a versioned
  native call that opens the shared browser for the activated reference.
- The terminal fragment: point the holotape at the browser program while
  retaining a recovery path.

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
- craft and place multiple TVs, projectors, screens, and video terminals;
- activate each kind and confirm the same browser/player design opens with the
  correct target name and capabilities;
- pair two projectors with two different screens, move them, save/load, scrap
  one screen, and repair only the broken pairing;
- run different files and timestamps on multiple placed outputs without state
  leaking between references;
- walk away from a playing world output and confirm audio remains spatial,
  menu input is released, and playback follows the configured distance policy;
- `EnableWorldScreens=0` to confirm every world object remains safely gated
  until this entire interaction path is ready.

## Completion criteria

The SWF work is complete only when a user can browse individual Movie and TV
files, start one, pause it, leave and resume at the same position, return to
the browser, and exit to a fully functional Pip-Boy without a blank screen,
stuck input, duplicate cursor, audio leak, or dependence on the vanilla
Pip-Boy's exact geometry. The same browser/player must open from every MMVP
television, projector, screen, and video terminal, control the correct placed
output, retain explicit pairings and per-output progress across saves, and
close without stopping unrelated world screens. OG, NG, and AE packages must
all still build, and the main-menu player must pass its existing regression
checks.
