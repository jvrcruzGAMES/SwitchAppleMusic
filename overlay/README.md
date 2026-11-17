# Music overlay

The layout follows sys-tune's compact status/transport approach
([reference](https://github.com/HookedBehemoth/sys-tune/blob/master/overlay/source/elm_status_bar.cpp))
with an original large-cover layout. A centered 256-pixel cover sits above track
metadata, progress and Previous / Play-Pause / Next vector icon buttons.

- Left/Right selects a transport button; A activates it.
- R opens synced lyrics; L returns to the player. These shoulder buttons have no playback action. Y toggles playback.
- Down moves to the seek bar, shuffle, repeat and stop controls below the player. Volume is adjusted in NRO Settings only.
- The transport buttons also accept touch taps.

The overlay remains IPC-only. The sysmodule prepares the current cover on a
separate worker; the backend resizes and converts it into libultrahand's RG/BA
nibble format. The overlay assembles validated 12 KiB chunks inside the existing
IPC message limit. It retains one 128 KiB cover buffer, reused on track changes.
Missing art leaves a music-note placeholder. Account/track changes clear stale
covers, and a failed image fetch does not stop audio.

The NRO has a separate bounded RGBA thumbnail cache. The overlay does not read
client credentials for a network connection and does not initialize sockets.
Both still use the saved backend session to authorize their IPC commands.

Native overlay rendering and touch behavior require a physical Switch; host
tests exercise the artwork worker, pixel format and chunk assembly, and the
Switch build compiles the actual libultrahand renderer and controls.
