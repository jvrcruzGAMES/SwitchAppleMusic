# SwitchMusic

An experimental three-part Nintendo Switch Apple Music client, with a self-hosted backend.

| Component | Implementation | Output |
| --- | --- | --- |
| Background player | libnx sysmodule + switch-libmpv, HOS audio renderer | `switchmusic-sys.nsp` |
| In-game controls | [libultrahand](https://github.com/ppkantorski/libultrahand) | `switchmusic.ovl` |
| Library interface | [dragonflylee/borealis](https://github.com/dragonflylee/borealis) | `SwitchMusic.nro` |
| Backend | Python/FastAPI, official Apple Music API, wrapper-v2 bridge, syncedlyrics | Docker image |

The NRO browses your library songs, albums, playlists, and their tracks. Selecting a song sends a queue to the sysmodule. Both interfaces control pause, previous/next, seek, volume, and stop. The sysmodule advances through subsequent library pages without keeping the NRO open. The NRO displays the current and upcoming lyric lines against mpv's playback position.

**Status:** all three native outputs cross-compile. Backend and framing tests pass. No Switch hardware or authenticated Apple Music account was available for end-to-end validation. The stock mpv/FFmpeg build produces a sysmodule with about **81 MiB of ELF text/data/BSS**, including a configurable 32 MiB heap, before extra thread/service allocations. A suitably sized system memory pool or a smaller audio-only mpv/FFmpeg build is required; this is not a verified drop-in module for a default Atmosphere installation. See [validation and remaining limitations](docs/VALIDATION.md).

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

CI and release builds run in the prebuilt `devkitpro/devkita64:latest` Docker image, using its installed toolchain and Switch portlibs without running `dkp-pacman` or synchronizing devkitPro package repositories.

The workflow builds all three Switch components and publishes a release containing **`sdout.zip`** and **`sdout.zip.sha256`**. Extract the ZIP directly to the SD root; it includes the `switch`, `atmosphere/contents/420000000000534D/flags`, and `config/switchmusic` folders. Your existing configuration is preserved; configure the supplied example for your backend. Boot activation remains manual as described below.

A new tag points to the selected workflow source commit. If the tag already exists, its exact commit is built instead. Existing releases are not overwritten. The workflow uses GitHub's built-in token and needs no additional repository secret. Creating/pushing a tag alone does not trigger this workflow.

## Host the backend

1. Set up [wrapper-v2](https://github.com/glomatico/wrapper-v2) and authenticate its subscribed playback account, including 2FA. Keep its HTTP and decrypt ports private; the Switch never connects to them.
2. Create a Media Services key enabled for MusicKit in your Apple Developer account. Save its `.p8` as `secrets/AuthKey.p8`. The private key stays on the server.
3. Copy `.env.example` to `.env`. Set `ADMIN_PASSWORD` (12–256 characters), `PUBLIC_URL` to the website's exact external origin, the Apple team/key IDs, wrapper addresses and playback storefront. Set `BACKEND_UID`/`BACKEND_GID` to the host owner of the private key and data directory. Run `mkdir -p data` to create the persistent account directory before starting Compose.
4. Run `docker compose up --build -d`. Put an HTTPS reverse proxy in front of localhost port 8000 and make the hostname reachable from the Switch. `SECURE_COOKIES=true` is the production default. For local HTTP testing only, set it to `false` and set `PUBLIC_URL` to the exact HTTP origin (including its port).
5. Open the website and sign in as **admin** using `ADMIN_PASSWORD`. Only admin can create accounts; there is no public registration. The Accounts tab lets admin create accounts, reset their passwords, enable/disable or delete them, and remove their Apple library links. Users can change their own passwords. Password changes, resets and disabling revoke browser sessions.
6. Each user signs into the website with their backend account and chooses **Link Apple Music**. Official MusicKit JS opens Apple's authorization flow; the backend validates the result with the official Apple Music API and stores the user's encrypted Music User Token. Linking is optional: accounts without a link can search and play public catalog tracks. A link enables that user's library, albums and playlists. No Apple password is collected by this application.
7. Open the SwitchMusic NRO and enter the server URL, backend username and password using the Nintendo Switch keyboard applet. The NRO saves an expiring session, not the password. Device tokens and mounted Music User Token secrets are no longer supported. Existing username/password configurations are migrated after signing in; no manual SD configuration is required.

`ADMIN_PASSWORD` bootstraps the admin only when the database is first created; changing the environment variable later does not reset its password. Change it through the website while signed in. Persist and back up **both** `data/accounts.sqlite3` and `data/apple-token.key` together. The key is needed to decrypt library links; protect the entire directory. Passwords are Argon2 hashes, and browser session tokens are stored as hashes. Apple revocation/expiry requires relinking on the website.

All playback uses the server's authenticated wrapper-v2 subscriber. Linking different Apple libraries does not create separate wrapper sessions or grant playback entitlements; catalog availability remains subject to that wrapper account and region. Catalog browsing and per-user library browsing use Apple's official API. Only canonical public catalog audio is shared in the bounded playback cache; private library tokens are never sent to the Switch. A short-lived developer JWT is necessarily delivered to the MusicKit browser, while the signing key remains server-side.

Synchronized lyrics are gathered through the `syncedlyrics` Python package. No Musixmatch API key is required. Only timed lyrics are requested; unavailable lyrics are reported without invented timing.

Compose runs under the configured non-root host UID/GID with a read-only root filesystem. Only the Apple private key is mounted as a secret; accounts and encrypted library links live in the persistent data volume. wrapper-v2 runs separately; `host.docker.internal` resolves to the Docker host on Linux. The image installs a pinned Gamdl revision for wrapper-v2 HLS/sample handling and never uses Widevine.

For local development:

```sh
python3 -m venv .venv
.venv/bin/pip install -e './backend[playback,test]'
# Export the values from .env and APPLE_PRIVATE_KEY=/absolute/path/to/AuthKey.p8.
# Use a writable DATA_DIR, PUBLIC_URL=http://localhost:8000 and SECURE_COOKIES=false.
.venv/bin/uvicorn switchmusic.app:app --host 127.0.0.1 --port 8000 --no-access-log
.venv/bin/pytest backend/tests -q
```

## Install on the Switch

Use an Atmosphere-capable Switch with a supported Ultrahand/nx-ovlloader setup. First verify the memory requirements for your console's sysmodule configuration.

1. Extract the SD-card archive to the SD root. It contains the NRO at `switch/SwitchMusic/SwitchMusic.nro`, the overlay at `switch/.overlays/switchmusic.ovl`, and the sysmodule at `atmosphere/contents/420000000000534D/exefs.nsp`, and its certificate RomFS at `atmosphere/contents/420000000000534D/romfs.bin`.
2. Launch the NRO in **application mode** (title takeover) and sign in with your server URL and backend account. Each input opens Nintendo’s software keyboard; password input is masked. A missing, expired or revoked login returns to this screen. Sessions expire after 12 hours; reopening the app validates the saved session, and it checks again every 30 seconds while open. The NRO automatically saves the URL, username and session in `config/switchmusic/config.json`. Account passwords are not saved. Use **Sign out** to remove the saved session and request server-side logout.
3. Open **Hekate Toolbox → Background services**, find **SwitchMusic**, and toggle it **On**. The packaged `atmosphere/contents/420000000000534D/toolbox.json` declares `requires_reboot: false`, so Toolbox can launch and terminate it without rebooting. Toolbox also updates `flags/boot2.flag` for the next boot; the archive initially omits this activation file. Configuration/startup failures are written to `config/switchmusic/fatal.log`. Without Toolbox, create `flags/boot2.flag` manually and reboot.
4. Browse a collection or search the catalog and select a song. The first song takes time to prepare: the backend materializes a temporary decrypted AAC/fMP4, then the sysmodule stages it on SD before mpv opens it. Two rotating SD files are limited to 64 MiB each. Playback stays in the sysmodule when the NRO closes.
5. Open the SwitchMusic overlay from Ultrahand to control the background player.

Both the NRO and sysmodule use the bundled Mozilla/curl CA trust store from RomFS, with TLS verification enabled. There is no `ca_bundle` setting or separate certificate download. Install the complete archive when upgrading so all three components use the same session-based control protocol. Protect the SD card: its saved session grants account access until expiration or revocation. The sysmodule can start before login; it and the overlay reload saved login changes automatically.

To disable the module immediately, toggle **SwitchMusic → Off** in Hekate Toolbox. This terminates the process, interrupts playback and pending work, and removes `flags/boot2.flag`. Toggle it **On** again to start a fresh process; the playback queue is not restored. The NRO/overlay reconnect on their next state poll. Merely deleting `boot2.flag` changes the next boot; it does not stop a currently running process. Without Toolbox, remove the flag and reboot. Stop the module before removing its title directory to uninstall it. Remove `config/switchmusic/audio-*.m4a` to clear staged audio. The backend's cache is bounded, expires after 30 minutes, and lives on Docker tmpfs; it disappears when the container is recreated.

The original project code is MIT licensed. Dependencies retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md). Apple Music, Nintendo Switch and Musixmatch are their respective owners' marks; this project is independent.

Architecture, control protocol, API and trust boundaries: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
