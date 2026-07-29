Put main-menu videos in this folder.

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
Avoid pairing an XWM with a BK2 that already contains audio unless you want
both tracks to play.

Default main-menu controls:

    Tab         Play another random video, including BK2
    Backspace   Stop video and audio
    Page Up     Volume up
    Page Down   Volume down

The upper-left help card displays these controls for five seconds. Their keys,
volume step, and display time can be changed in:

    Data\F4SE\Plugins\MainMenuVideoPlayer.ini

MMVP keeps its packaged black MainMenuLoop.bk2 open as Fallout's stable
carrier. A selected BK2 is opened through Fallout's Bink decoder and drawn
over that carrier. Tab can therefore switch between BK2 and ordinary videos
as many times as desired during the same menu session.
