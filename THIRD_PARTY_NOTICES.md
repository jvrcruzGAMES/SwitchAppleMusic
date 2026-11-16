# Third-party notices

Original SwitchMusic code is covered by the root MIT license. That license does not replace dependency licenses or grant rights to provider media.

- `third_party/borealis`: dragonflylee's Borealis fork, pinned Git submodule. See its Apache-2.0 license and licenses of nested submodules/resources.
- `third_party/libultrahand`: ppkantorski's libultrahand, pinned Git submodule. See its GPL-2.0 license and CC-BY-4.0 `SUB_LICENSE`; preserve its attribution and file notices.
- `third_party/json`: nlohmann/json 3.12.0 single header, MIT. Included license: `third_party/json/LICENSE.MIT`.
- `resources/certs/cacert.pem`: unmodified Mozilla CA trust store converted by [curl](https://curl.se/docs/caextract.html), MPL-2.0. The license is included at `resources/certs/LICENSE.txt` and in the SD archive. Snapshot/source information is in `resources/certs/README.md`.
- devkitPro/libnx, FFmpeg, and linked portlibs: supplied by the toolchain, not vendored here; their own licenses apply to linked distributions. Retain appropriate notices and corresponding source when distributing binaries under applicable dependency terms.
- Audio-output design reference: [HookedBehemoth/sys-tune](https://github.com/HookedBehemoth/sys-tune), commit `24132cfa26e569011a2028790575cd474f32cf95` (GPL-2.0). SwitchMusic independently implements its recycled Horizon audout buffer pattern with our own FFmpeg decoder and transport.
- Backend synchronized lyrics use moehmeni/syncedlyrics, MIT, with its own provider dependencies.
- Backend playback uses a pinned revision of glomatico/Gamdl, MIT, including its Rust dependencies. wrapper-v2 is a separately hosted dependency under its upstream license.

Apple Music Android libraries are supplied separately by the host operator according to wrapper-v2's setup; they are not included. Apple Music API keys and lyric content are not redistributable project assets. Lyrics are retrieved through syncedlyrics; provider content retains its owners' rights.
