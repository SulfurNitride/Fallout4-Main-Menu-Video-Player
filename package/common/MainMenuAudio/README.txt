Optional dedicated main-menu soundtracks go in this folder.

Supported sources include standalone MP3, AAC/M4A, FLAC, OGG, Opus, WAV,
WMA, XMA, and XWM files, plus audio streams inside MP4, MKV, WebM, AVI, MOV,
WMV, and BK2 containers. MMVP ignores any video stream and uses the first
decodable audio stream.

Give an audio file the same basename as a video to pair them. A matching file
replaces that video's embedded audio. If no matching file exists, MMVP keeps
decodable embedded audio; for a silent video it chooses a random file from this
folder without immediately repeating the previous choice.

N plays another randomized soundtrack. M switches between dedicated audio and
the video's original audio when both are available. Both keys can be changed or
disabled in MainMenuVideoPlayer.ini.
