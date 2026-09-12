Put main-menu videos in this folder.

The FOMOD provides main-menu playback without an ESP, ESL, or holotape.

The player currently accepts:
3g2, 3gp, asf, avi, f4v, flv, m4v, mkv, mov, mp4, mpeg, mpg,
ogv, qt, vob, webm, wmv, and native bk2 Bink videos.

One file is selected randomly whenever a new main-menu session starts. All
formats share one shuffle pool. When more than one file exists, the player
avoids selecting the same file twice in a row.

To give a silent Bink an external soundtrack, place an XWM beside it with the
same basename:

    AwesomeVideo.bk2
    AwesomeVideo.xwm

The XWM starts with the Bink, loops at its end, and uses MMVP's volume keys.
Audio embedded directly in a BK2 uses the same volume keys. MMVP asks Bink for
every selected file's actual audio-track IDs instead of assuming track numbers,
so BK2 files with different track layouts remain safe. When an XWM sidecar is
present, it takes priority: MMVP keeps the BK2 audio clock running but sets
every embedded track to zero volume, then plays only the XWM soundtrack.
If MainMenuAudio contains a supported file with the same basename as the video,
that paired soundtrack takes priority. Without a same-name file, MMVP keeps the
video's embedded audio; a silent video receives a random MainMenuAudio track.
M restores the video's own embedded/original audio stream when one exists.

Default main-menu controls:

    Tab         Play another random video, including BK2
    Backspace   Stop video and audio
    Page Up     Volume up
    Page Down   Volume down
    N           Next dedicated soundtrack
    M           Toggle dedicated/original audio

The upper-left help card displays these controls for five seconds. Their keys,
volume step, and display time can be changed in:

    Data\F4SE\Plugins\MainMenuVideoPlayer.ini

When MatchWindowAspect=1, MMVP uses a BK2 from this folder whose aspect ratio
matches Fallout's window as the stable main-menu carrier. This avoids fitting
an ultrawide video inside the packaged 16:9 carrier before Fallout fits it
again. Without a matching BK2, the packaged black MainMenuLoop.bk2 remains the
fallback, and non-16:9 videos may still appear letterboxed. Set
MatchWindowAspect=0 if another menu mod already handles aspect correction.
The selected BK2 is opened separately through Fallout's Bink decoder and
drawn over the carrier. Tab can still switch between BK2 and ordinary videos.

Bicubic is the default auto-threaded scaling algorithm. Spline36 can be
selected in MainMenuVideoPlayer.ini for higher-quality zscale resizing at the
cost of higher temporary CPU and system-memory use. Scaling resources are
released when the main menu closes.
