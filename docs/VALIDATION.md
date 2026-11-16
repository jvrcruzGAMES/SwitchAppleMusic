# Validation

## Verified in the development workspace

- Cross-compiled and linked `SwitchMusic.nro`, `switchmusic.ovl`, and `switchmusic-sys.nsp` using devkitA64 GCC 16.1.0 and the installed libnx/portlibs.
- Checked the overlay's trailing `ULTR` signature, NRO headers, NSP PFS0 entries, NSO header and NPDM program ID during packaging. Toolbox metadata is also validated against that ID and must declare live load/unload support.
- The 61 backend tests exercise admin-only account creation, cookie CSRF/origin checks, password/session revocation, encrypted per-user library isolation, single-use MusicKit linking, catalog access without linking, persistence across restarts, and pagination, input validation, official Apple request headers, sanitized resource output, library-to-catalog resolution, keyless syncedlyrics lookup, LRC timestamps, HTTP byte ranges, serialized cache preparation, failure cleanup and cache budget eviction. Upstreams are mocked; these tests do not claim live account validation.
- Host C++ framing test covers round trips, fragmented frames, invalid JSON, maximum size enforcement and URL escaping.
- Built the pinned Gamdl Rust/Python package under Python 3.12 and imported the adapter's API/model/stream classes. Its 17 synthetic wrapper-v2 TCP/fMP4 fixture tests passed.
- Built the backend Docker image and smoke-tested non-root startup, a read-only root filesystem, native playback imports, HTTP health, unauthenticated route rejection, packaged web assets, account creation and account/session persistence across container restarts. Docker Compose configuration also parsed successfully.

The SDK here was missing GLM and libjpeg. They were built/staged under `/tmp` for validation; the installed devkitPro tree was not changed. Normal builds should install `switch-glm` and `switch-libjpeg-turbo`. The JPEG test build used PIC and the Switch ARMv8/soft-TLS ABI flags. Host packages must not enter the Switch pkg-config search path.

## Hardware and account acceptance still needed

1. Measure the actual sysmodule memory footprint and available system pool before enabling boot activation. The stock video-capable mpv/FFmpeg archive contributes about 49 MiB beyond the configured 32 MiB heap; a reduced audio-only library is preferable for coexistence with other modules. No memory-pool patch is supplied or applied.
2. Verify module startup, HOS audio renderer initialization, audio mixing while a game runs, headphone/dock transitions, sleep/resume, and clean reboot/uninstall on the target Atmosphere/Horizon versions.
3. Authenticate a real subscribed wrapper-v2 account. Create backend users through the admin website and link their libraries using MusicKit. Verify library isolation, catalog-only access, and official library/catalog requests. Ensure the wrapper playback region supports the selected tracks. The interactive Apple authorization flow has not been exercised with a real account.
4. Play a real wrapper AAC rendition, exercise seek/pause/skip/volume, run through a multi-page playlist after closing the NRO, and verify failure reporting after network loss or expired account authorization.
5. Verify live `syncedlyrics` searches and lyric synchronization across tracks and provider availability; no Musixmatch API key is required.
6. In Hekate Toolbox, repeatedly toggle SwitchMusic On → Off → On while idle, playing and preparing a track. Verify process termination, audio renderer release, port rebinding, UI reconnection and `boot2.flag` persistence. Launch success remains subject to the memory constraint above.
7. Visually validate the Borealis screens, controller/touch navigation, long track names and Ultrahand controls on-device. No emulator screenshots or simulated UI are presented as hardware evidence.

## Current behavior and limits

Multiple local accounts with optional per-user Apple library links, public catalog search, read-only library browsing, shared host wrapper playback, bounded queue history, track staging latency, and no gapless/prefetch/shuffle/repeat/offline library. Album art is not implemented. The web UI could not be visually checked because the browser automation host was unavailable; HTTP/static asset behavior is covered by tests. Uploaded library tracks without catalog matches and music videos are unavailable. No Apple authentication secrets are shipped with the project, and live service operation requires your host configuration and account entitlements.
