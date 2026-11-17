# Nintendo Switch interface

The NRO uses Borealis XML in `resources/xml/switchmusic/`. Switchfin’s sidebar,
XML music rows and dedicated transport panel informed the navigation and layout:
[main shell](https://github.com/dragonflylee/switchfin/blob/dev/resources/xml/activity/main.xml),
[music rows](https://github.com/dragonflylee/switchfin/blob/dev/resources/xml/view/music_track.xml),
[player](https://github.com/dragonflylee/switchfin/blob/dev/resources/xml/view/music_view.xml).
The layouts and colors here are original; Switchfin is not a runtime dependency.

| Resource | Purpose |
| --- | --- |
| `shell.xml` | Persistent sidebar, account badge, status and scrolling page host |
| `home.xml`, `search.xml` | Discovery, library shortcuts and catalog search |
| `login.xml`, `settings.xml` | Server/account inputs and Apple library settings |
| `collection.xml`, `row.xml` | Collection headers, empty/pagination states and reusable music rows |
| `player.xml` | Metadata, progress, transport/mode controls and synced lyrics |
| `qr.xml` | Apple linking instructions, confirmation, timer and QR placement |
| `message.xml` | Loading and recoverable error states |

C++ binds backend HTTP data, Horizon IPC playback controls, click/controller actions, visibility and state. Repeated
music rows inflate `row.xml`; the queue snapshot is shared between their actions.
Custom rendering covers the QR matrix (`SwitchMusicQR`) and small vector transport
glyphs (`SwitchMusicTransportIcon`); their sizing and placement still come from XML. Borealis exposes theme tokens through a
C++ API, so the small global accent palette is set before XML inflation.
The Nintendo Switch keyboard applet handles all text/password entry; the desktop
preview uses Borealis’s desktop IME.

D-pad/stick moves focus, A selects, B returns, X opens Search and Y toggles playback
when signed in. L/R skips tracks on the player. The sidebar stays available on all
pages. Collection history retains up to 16 locations and restores the selected
row and scroll position on return. Centered scrolling moves focus immediately to
offscreen rows with D-pad input. Focus is assigned after Yoga layout so controls
below the fold are revealed correctly. Guest and unlinked accounts do not display
personal-library navigation. Login and Apple linking requirements are unchanged.

XML resources are copied into NRO RomFS by a dependency-tracked CMake target;
editing a layout recopies it and rebuilds the NRO archive. The SD archive needs no
external UI files. This does not change the sysmodule’s decoder or memory budget.

## Desktop rendering and controller checks

With SDL2 development libraries, OpenGL/Mesa, libcurl, a C++ compiler and a display:

```sh
cmake -S tests/ui -B build-ui -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ui -j4
python3 scripts/test-nro-ui.py --binary build-ui/switchmusic-ui-preview --screenshots dist/ui-preview
```

For a headless host, run the Python command through `xvfb-run -a`. The fixture serves
only synthetic account/catalog data from loopback and uses a temporary directory
for native configuration. All screens are inflated and rendered by the production
UI code. Checks include long-title column bounds, empty collections, sidebar focus
routes, X/B shortcuts, collection back history, selected-row restoration (including returning from the player) and guest
library visibility. Optional PPM screenshots come directly from the OpenGL
framebuffer. Switch keyboard, touchscreen and physical Joy-Con checks still need
hardware.

`row.xml` places the order number, a 52-pixel cover view, and the title/artist
column in that order. Covers are requested as 64-pixel RGBA4444 data through the backend
on the existing network worker. Page changes cancel stale image jobs; texture
updates happen on the render thread. The 64-entry RGBA8 thumbnail cache (1 MiB) is scoped
to backend/session and cleared at account/guest transitions. Each page owns and
releases its textures. A music-note placeholder remains when artwork is absent.

Now Playing uses a two-column XML layout: cover/metadata, icon transport buttons,
a seekable progress bar, shuffle/repeat and stop on the left; a full-height synced-lyrics panel on the right.
The active line is centered between the previous and next lines. Play/pause changes
its vector glyph with playback state, retaining the standard button focus and
controller bindings. The current cover uses 256-pixel backend RGBA4444 data and is cleared
when the track changes; full-size covers do not enter the thumbnail cache.
The desktop player fixture checks actual artwork loading, lyric placement, icon
state, transport focus and the empty-player reset.

Focus the progress bar with the D-pad, use Left/Right to preview a seek in
five-second steps, then A to apply or B to cancel. Touch taps and horizontal drags
seek on release. The overlay has a separate focusable progress-bar row below the transport
icons. Press Down to reach it, then use the same seek/apply/cancel controls.
Press Up to return to the icons, or Down again to reach shuffle/repeat.
Shuffle and repeat state are shared by both interfaces. Repeat cycles Off → Queue
→ Song → Off. Manual Next still skips when repeat-song is selected. Volume controls
are removed; the sysmodule always opens audio output at the previous 70% default.

The app now uses the same RGBA4444 wire format as the working overlay. The network
worker expands color/alpha nibbles into RGBA8; the render thread uploads a NanoVG
texture and explicitly refreshes the image paint after asynchronous loading.
Older backends that reject 64-pixel raw artwork fall back to 256-pixel raw artwork,
downsampled to 64 pixels before caching. Updating the backend enables smaller
thumbnail responses but is not required for compatibility with the previous release.
Artwork failures retry up to three attempts and record the latest path/reason in
`sdmc:/config/switchmusic/artwork.log`; account credentials are not logged.
