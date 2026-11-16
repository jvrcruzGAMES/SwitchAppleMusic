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
| `player.xml` | Metadata, progress, transport/volume controls and synced lyrics |
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
column in that order. Covers are requested as 64-pixel PNGs through the backend
on the existing network worker. Page changes cancel stale image jobs; texture
updates happen on the render thread. The 64-entry encoded-image cache is scoped
to backend/session and cleared at account/guest transitions. Each page owns and
releases its textures. A music-note placeholder remains when artwork is absent.

Now Playing uses a two-column XML layout: cover/metadata, icon transport buttons,
seek and volume on the left; a full-height synced-lyrics panel on the right.
The active line is centered between the previous and next lines. Play/pause changes
its vector glyph with playback state, retaining the standard button focus and
controller bindings. The current cover uses a 256-pixel backend PNG and is cleared
when the track changes; full-size covers do not enter the thumbnail cache.
The desktop player fixture checks actual artwork loading, lyric placement, icon
state, transport focus and the empty-player reset.
