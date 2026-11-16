# Third-party notices

Original SwitchMusic code is covered by the root MIT license. That license does not replace dependency licenses or grant rights to provider media.

- `third_party/borealis`: dragonflylee's Borealis fork, pinned Git submodule. See its Apache-2.0 license and licenses of nested submodules/resources.
- `third_party/libultrahand`: ppkantorski's libultrahand, pinned Git submodule. See its GPL-2.0 license and CC-BY-4.0 `SUB_LICENSE`; preserve its attribution and file notices.
- `third_party/json`: nlohmann/json 3.12.0 single header, MIT. Included license: `third_party/json/LICENSE.MIT`.
- devkitPro/libnx, switch-libmpv/mpv, FFmpeg, and linked portlibs: supplied by the toolchain, not vendored here; their own licenses apply to linked distributions. Retain appropriate notices and corresponding source when distributing binaries under applicable dependency terms.
- Backend playback uses a pinned revision of glomatico/Gamdl, MIT, including its Rust dependencies. wrapper-v2 is a separately hosted dependency under its upstream license.

Apple Music Android libraries are supplied separately by the host operator according to wrapper-v2's setup; they are not included. Apple Music API keys and Musixmatch credentials/content are not redistributable project assets. Musixmatch subtitle availability and display/reporting rules depend on the operator's agreement.
