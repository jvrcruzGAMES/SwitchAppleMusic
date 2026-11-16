# SwitchMusic

An experimental three-part Nintendo Switch Apple Music client, with a self-hosted backend.

| Component | Implementation | Output |
| --- | --- | --- |
| Background player | libnx sysmodule + switch-libmpv, HOS audio renderer | `switchmusic-sys.nsp` |
| In-game controls | [libultrahand](https://github.com/ppkantorski/libultrahand) | `switchmusic.ovl` |
| Library interface | [dragonflylee/borealis](https://github.com/dragonflylee/borealis) | `SwitchMusic.nro` |
| Backend | Python/FastAPI, official Apple Music API, wrapper-v2 bridge, Musixmatch | Docker image |

The NRO browses your library songs, albums, playlists, and their tracks. Selecting a song sends a queue to the sysmodule. Both interfaces control pause, previous/next, seek, volume, and stop. The sysmodule advances through subsequent library pages without keeping the NRO open. The NRO displays the current and upcoming lyric lines against mpv's playback position.

**Status:** all three native outputs cross-compile. Backend and framing tests pass. No Switch hardware or authenticated Apple/Musixmatch account was available for end-to-end validation. The stock mpv/FFmpeg build produces a sysmodule with about **81 MiB of ELF text/data/BSS**, including a configurable 32 MiB heap, before extra thread/service allocations. A suitably sized system memory pool or a smaller audio-only mpv/FFmpeg build is required; this is not a verified drop-in module for a default Atmosphere installation. See [validation and remaining limitations](docs/VALIDATION.md).

## Low-memory FFmpeg build (Option 2)

A separate audio-only Switch FFmpeg SDK now supports **AAC, AAC-web and ALAC**, with Atmos/AC-3 excluded. Its cross-linked decoder + HTTP/control + HOS-audio fixture is **11.92 MB**, including an 8 MiB heap. Adding a 4 MiB platform allowance gives a **16.11 MB planning budget**, below the 22 MB target and 36.6 MB maximum. This is not an on-device peak measurement, and the existing mpv sysmodule has not been replaced by this SDK.

```sh
python3 scripts/build-audio-ffmpeg.py --target switch
python3 scripts/check-audio-budget.py
python3 scripts/build-audio-ffmpeg.py --target host
python3 scripts/test-audio-ffmpeg.py
python3 scripts/package-audio-ffmpeg.py
```

The backend prefers **ALAC → AAC → AAC-web**, using wrapper-v2 only. See [the profile, measurements and integration boundaries](docs/AUDIO_FFMPEG.md). The SDK archive is `dist/switch-ffmpeg-audio-7.1-switchmusic.tar.gz`.

## Build

Install devkitPro, devkitA64, libnx, switch-tools, CMake, and the Switch packages below. libultrahand currently requires a compiler supporting C++26.

```sh
(dkp-)pacman -S switch-dev switch-cmake switch-curl switch-zlib switch-minizip \
  switch-mbedtls switch-glm switch-libmpv switch-libplacebo switch-libjpeg-turbo
```

`(dkp-)pacman` means use your installation's `dkp-pacman` or `pacman` command. mpv's package also needs its FFmpeg, SDL2, EGL, libass and other transitive dependencies. Dependencies must be Switch builds, not host libraries.

```sh
git submodule update --init --recursive
export DEVKITPRO=/opt/devkitpro
cmake -S . -B build-switch -DCMAKE_BUILD_TYPE=Release
cmake --build build-switch -j4
python3 scripts/package.py --build build-switch
```

Outputs are in `build-switch/`; the SD-card archive is `dist/SwitchMusic-sdcard.zip`. Change the heap with `-DSWITCHMUSIC_HEAP_MB=32` after measuring your build. The sysmodule uses audio only (`vo=null`, `video=no`, `ao=hos`).

## Create a GitHub release

Open **Actions → Release SD card package → Run workflow**. Select the source branch, enter the desired **tag** (for example `v0.1.0`), optionally select **prerelease**, and run it.

The workflow builds all three Switch components and publishes a release containing **`sdout.zip`** and **`sdout.zip.sha256`**. Extract the ZIP directly to the SD root; it includes the `switch`, `atmosphere/contents/420000000000534D/flags`, and `config/switchmusic` folders. Your existing configuration is preserved; configure the supplied example for your backend. Boot activation remains manual as described below.

A new tag points to the selected workflow source commit. If the tag already exists, its exact commit is built instead. Existing releases are not overwritten. The workflow uses GitHub's built-in token and needs no additional repository secret. Creating/pushing a tag alone does not trigger this workflow.

## Host the backend

This release is **single-account**: the wrapper session is the Apple Music account served to every device bearing your device token. Use one isolated deployment per account.

1. Set up [wrapper-v2](https://github.com/glomatico/wrapper-v2) using its instructions and your own compatible Apple Music Android libraries. Authenticate its account and complete 2FA through its private HTTP API. Keep its HTTP and decrypt ports on a trusted host/network. The Switch does not connect to those ports.
2. Create a Media Services key enabled for MusicKit in your Apple Developer account. Save its `.p8` as `secrets/AuthKey.p8`. The server signs its own ES256 developer tokens and sends library requests to `https://api.music.apple.com/v1/` with `Authorization` and `Music-User-Token` headers. Obtain your own Music User Token with the local MusicKit helper:

   ```sh
   python3 -m venv .venv
   .venv/bin/pip install -e ./backend
   .venv/bin/python scripts/authorize-musickit.py --team-id YOUR_TEAM_ID --key-id YOUR_KEY_ID
   ```

   Open the printed localhost URL in your computer browser and authorize the same account used by wrapper-v2. The helper writes `secrets/music_user_token` with mode 0600 and exits. MusicKit receives a short-lived developer JWT in that browser; the private key stays on the host. No Apple password, private key, developer JWT or user token is returned to the Switch.
3. Copy `.env.example` to `.env`, set the Apple team/key IDs, wrapper addresses, storefront, `BACKEND_UID`/`BACKEND_GID` matching the secret files' host owner, and a random device token of at least 32 characters. The host account's storefront and `APPLE_STOREFRONT` must match. Generate the device token with `python3 -c 'import secrets; print(secrets.token_urlsafe(32))'`.
4. Set `MUSIXMATCH_API_KEY` to a key whose plan grants access to synchronized subtitles. The backend uses the official `matcher.subtitle.get` endpoint, matching by ISRC where available. Missing access is reported explicitly. Plain lyrics are not fabricated into synchronized lines.
5. Run `docker compose up --build -d`. Compose binds the backend to localhost by default; put an HTTPS reverse proxy in front of port 8000 and make its hostname reachable from your Switch. For a private LAN test you can set `BIND_ADDRESS` to the host's LAN address and use HTTP. Transport security is determined by that URL.

The Apple private key and Music User Token are mounted as Docker secrets. Compose runs under the configured non-root host UID/GID so mode-0600 secret files remain readable without making them public. wrapper-v2 runs separately; `host.docker.internal` resolves to the Linux Docker host in the supplied Compose configuration. Change the addresses if wrapper is on another host/network. The backend image installs a pinned Gamdl revision (including its Rust extension) for wrapper-v2 HLS/sample handling. Catalog and library browsing use Apple's official API; the playback adapter selects only wrapper-supplied HLS sources and rejects unhandled encrypted output.

For local development:

```sh
python3 -m venv .venv
.venv/bin/pip install -e './backend[playback,test]'
# Export the variables in .env, APPLE_PRIVATE_KEY=/absolute/path/to/AuthKey.p8,
# and APPLE_MUSIC_USER_TOKEN_FILE=/absolute/path/to/music_user_token
.venv/bin/uvicorn switchmusic.app:app --host 127.0.0.1 --port 8000 --no-access-log
.venv/bin/pytest backend/tests -q
```

## Install on the Switch

Use an Atmosphere-capable Switch with a supported Ultrahand/nx-ovlloader setup. First verify the memory requirements for your console's sysmodule configuration.

1. Extract the SD-card archive to the SD root. It contains the NRO at `switch/SwitchMusic/SwitchMusic.nro`, the overlay at `switch/.overlays/switchmusic.ovl`, and the sysmodule at `atmosphere/contents/420000000000534D/exefs.nsp`.
2. Copy `config/switchmusic/config.example.json` to `config/switchmusic/config.json`. Set `backend_url` and the same `device_token` as the backend. For HTTPS, copy a current CA PEM bundle to `config/switchmusic/cacert.pem` (or update `ca_bundle`). TLS verification stays enabled. Never put the `.p8`, Apple account credentials or Musixmatch key on the SD card.
3. Open **Hekate Toolbox → Background services**, find **SwitchMusic**, and toggle it **On**. The packaged `atmosphere/contents/420000000000534D/toolbox.json` declares `requires_reboot: false`, so Toolbox can launch and terminate it without rebooting. Toolbox also updates `flags/boot2.flag` for the next boot; the archive initially omits this activation file. Configuration/startup failures are written to `config/switchmusic/fatal.log`. Without Toolbox, create `flags/boot2.flag` manually and reboot.
4. Launch SwitchMusic through hbmenu in **application mode** (title takeover), browse a collection and select a song. The first song takes time to prepare: the backend materializes a temporary decrypted AAC/fMP4, then the sysmodule stages it on SD before mpv opens it. Two rotating SD files are limited to 64 MiB each. Playback stays in the sysmodule when the NRO closes.
5. Open the SwitchMusic overlay from Ultrahand to control the background player.

To disable the module immediately, toggle **SwitchMusic → Off** in Hekate Toolbox. This terminates the process, interrupts playback and pending work, and removes `flags/boot2.flag`. Toggle it **On** again to start a fresh process; the playback queue is not restored. The NRO/overlay reconnect on their next state poll. Merely deleting `boot2.flag` changes the next boot; it does not stop a currently running process. Without Toolbox, remove the flag and reboot. Stop the module before removing its title directory to uninstall it. Remove `config/switchmusic/audio-*.m4a` to clear staged audio. The backend's cache is bounded, expires after 30 minutes, and lives on Docker tmpfs; it disappears when the container is recreated.

The original project code is MIT licensed. Dependencies retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md). Apple Music, Nintendo Switch and Musixmatch are their respective owners' marks; this project is independent.

Architecture, control protocol, API and trust boundaries: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
