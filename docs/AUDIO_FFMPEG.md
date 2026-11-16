# Low-memory Switch FFmpeg profile

This is the isolated FFmpeg build for the Option 2 player: native AAC and ALAC decoding, without mpv. AAC-web uses the same AAC bitstream decoder; its delivery/decryption happens on the backend. Atmos/E-AC-3 and AC-3 decoders are excluded.

## Production player and memory

The `switchmusic-sys` target now links this profile directly and uses Horizon `audout`; libmpv is removed. The output design follows [sys-tune](https://github.com/HookedBehemoth/sys-tune/blob/24132cfa26e569011a2028790575cd474f32cf95/sys-tune/source/impl/music_player.cpp): a small fixed set of page-aligned PCM buffers is reused only after the audio service releases it. SwitchMusic implements this pattern independently, with four 4096-frame stereo buffers (64 KiB total).

The actual packaged NSO currently has approximately **12.05 MB** of page-aligned loadable sections and BSS, including a fixed **8 MiB heap**. Adding a separate **4 MiB platform allowance** gives approximately **16.25 MB**, below the decimal 22,000,000-byte target and 36,600,000-byte hard limit. `scripts/package.py` checks the actual NSO and includes `SwitchMusic-memory.json` in the archive; build-dependent exact values are recorded there. The heap already includes dynamic decoder, HTTP/TLS and PCM allocations. These are linked-image/planning measurements, not a measured Horizon runtime peak or a guaranteed upper bound for kernel/service memory.

Socket buffers are fixed at 16 KiB TCP transmit / 128 KiB receive, 4 KiB UDP transmit/receive, efficiency 2 and two BSD sessions. libnx's transfer-memory formula gives 304 KiB rather than its default pool of over 2 MiB. This pool is allocated from the existing heap. One 256 KiB remote byte-range cache, one 32 KiB AVIO buffer, a 16 KiB PCM conversion scratch buffer, and four 16 KiB output slots bound player-side buffering. The decoder fixture/budget probe remains available as an isolated SDK check, separately from the production package check.

The backend generates authenticated, single-file fMP4 VOD playlists. Custom AVIO reconstructs the complete seekable MP4 through bounded HTTP Range requests, with stable ETag checks; no audio is staged on the SD card. This is a controlled backend protocol, not a general-purpose HLS client for arbitrary external manifests. Initial wrapper decryption and codec-copy host fragmentation finish before the first manifest is returned. Codec/sample rate/bit depth remain unchanged on the host; the Switch resamples to the platform's 48-kHz stereo output.

## Build and verify

```sh
export DEVKITPRO=/opt/devkitpro
python3 scripts/build-audio-ffmpeg.py --target switch
python3 scripts/check-audio-budget.py

# Build the identical codec/table profile for host execution.
python3 scripts/build-audio-ffmpeg.py --target host
python3 scripts/test-audio-ffmpeg.py
python3 scripts/test-player.py
python3 scripts/package-audio-ffmpeg.py
```

Required host tools: Python 3.11+, patch, make, a C compiler, and a full host `ffmpeg` executable for generating fixtures. Switch builds require devkitA64 and libnx; the budget probe additionally links the existing Switch curl/zlib packages. The build installs into `build-ffmpeg/switch`, leaving `/opt/devkitpro` untouched. Source downloads and the upstream Switch patch are SHA-256 verified; build metadata records the exact flags and patch fingerprint.

The isolated host decoder tests compare PCM against a full reference FFmpeg, allowing at most one 16-bit quantization step because reference FFmpeg versions can differ. Tests cover progressive AAC at 44.1/48 kHz, fragmented-MP4 AAC, 16-bit ALAC, 24-bit/192-kHz ALAC, fragmented ALAC, and AAC/ALAC fMP4 HLS playlists. A host allocation guard caps tracked FFmpeg allocations at 8 MiB. In the synthetic fixtures the highest tracked allocation peak was approximately 1.03 MB; these short fixtures are not a worst-case workload or a Switch heap measurement. The tests also reject AC-3 and verify AAC transform sizes, including 960/120-sample variants. Fixture MP4 movie timescales match the source sample rate, preserving exact edit durations on older FFmpeg muxers as well. PCM sample counts must still match exactly; this avoids container-duration rounding without weakening the decoder comparison.

## Configuration and table changes

- FFmpeg 7.1, with the pinned devkitPro Horizon port patch.
- Exactly two decoders: `aac`, `alac`. No encoders, video decoders, hardware-video backends, libavfilter, libswscale, libavdevice, postprocessing, external codec libraries or GPU libraries.
- libavformat, libavcodec, libavutil and libswresample only.
- MP4/fMP4 (`mov`), AAC and HLS demuxing. FFmpeg enables a few additional HLS prerequisite demuxers/parsers, but no AC-3/E-AC-3 decoder is enabled.
- Network protocol implementations disabled. The `file` protocol supports host fixtures; production network transport must supply bounded custom AVIO callbacks. TLS/HTTP remain outside FFmpeg.
- Native ARMv8/Cortex-A57/NEON, static/PIC, section garbage collection, size optimization, no debug/unwind tables.
- Split-radix FFT tables/codelets above **131072** removed. `av_tx_init` rejects larger transform requests, preventing access past the remaining initialization tables. This preserves all AAC transform sizes and the existing AArch64 NEON transform range. It removes oversized static buffers rather than depending on linker garbage collection to discard referenced tables.

The decoder harness uses one codec thread, a 256 KiB probe limit, a 128 KiB index limit, one packet/frame at a time, and a 16 KiB PCM conversion buffer. It validates source rates up to 192 kHz and up to eight input channels, then converts to 48-kHz stereo PCM in bounded chunks. The production player applies these limits and adds a 2 MiB limit per FFmpeg allocation.

`toolchain/AudioFFmpeg.cmake` supplies the `SwitchMusic::AudioFFmpeg` interface target for the direct player. Set `SWITCHMUSIC_AUDIO_FFMPEG_PREFIX` to this isolated prefix. Never allow host or stock Switch FFmpeg archives to precede it in the link search path.

## Backend selection

The exact preference order is **ALAC → AAC → AAC-web**. Gamdl's generic ALAC preference includes its lossless/high-resolution ALAC tiers. Atmos, AC-3, binaural and other variants are excluded from the selection policy. AAC-web is not converted to a different codec or relabeled as AAC HLS.

All decryption stays on wrapper-v2; no Widevine or PlayReady backend/extra is installed or invoked. The pinned Gamdl 4.0 stream adapter normally excludes web-playback streams from wrapper eligibility, so AAC-web with actual FairPlay `skd://` metadata is adapted to its generic wrapper-audio path while preserving the original manifest and codec. A missing compatible key URI is reported as an error; no URI is fabricated and encrypted output is never passed through. Live AAC-web account playback still needs verification against the operator's working wrapper setup.

The backend keeps a bounded server-side cache, including temporary remux space. Its per-track limit is now 256 MiB to accommodate ALAC; the default total remains 512 MiB. `MAX_TRACK_BYTES` and `MAX_CACHE_BYTES` can override those host limits. Codec priority falls back when a rendition is unavailable, not after authorization/decryption errors or a cache-limit failure.

## Source provenance

- [FFmpeg 7.1](https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz), SHA-256 `40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6`.
- [devkitPro Switch port patch](https://github.com/devkitPro/pacman-packages/blob/5c46517a8a1a1f0939a90b8068655ea00fa125b9/switch/ffmpeg/ffmpeg-7.1.patch), SHA-256 `1792380b992e3554a4abcddf0d7b395bfd8c118ac7c6e38c8f2fb0d39753a390`.
- Local table restriction: `toolchain/patches/ffmpeg-7.1-audio-tables.patch`.

FFmpeg retains its upstream license terms; the binary archive includes its license texts, recipe and patches. The companion source archive contains the patched source used for this build.
