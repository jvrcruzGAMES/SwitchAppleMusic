"""The console's complete codec policy; delivery names are not decoder names."""
from fastapi import HTTPException

CODEC_PRIORITY = ("alac", "aac", "aac-web")
SUPPORTED_SONG_CODECS = frozenset({"alac", "alac-lossless", "alac-hires", "aac", "aac-web"})

async def select_source(song, wrapper, codec=None):
    from gamdl.media import SongCodec
    priority = CODEC_PRIORITY[CODEC_PRIORITY.index(codec) if codec else 0:]
    source = await song.get_stream(
        codec_priority=tuple(SongCodec(candidate) for candidate in priority),
        wrapper_api=wrapper,
    )
    codec = source.codec.value
    if codec not in SUPPORTED_SONG_CODECS:
        raise HTTPException(502, "Selected codec is unsupported by the console")
    return source


async def wrapper_source(source):
    """Adapt web AAC with real FairPlay signaling to the pinned wrapper engine.

    Gamdl 4.0 excludes WEBPLAYBACK from wrapper eligibility. Preserve the actual
    manifest/key metadata while using its supported generic wrapper audio type.
    Never invent a key URI or pass encrypted bytes through unchanged.
    """
    if source.codec.value != "aac-web":
        return source
    from dataclasses import replace
    from gamdl.media import AudioStream, StreamOrigin
    from gamdl.media.stream import StreamTransport

    info = await source.get_drm_info()
    uri = info.fairplay_streaming or info.untyped_key_uri
    if not isinstance(uri, str) or not uri.startswith("skd://") or len(uri) <= 6:
        raise HTTPException(502, "AAC-web stream has no wrapper-compatible FairPlay key URI")
    signaling = replace(info, fairplay_streaming=uri)

    class WrapperWebAudio(AudioStream):
        async def get_manifest(self):
            return await source.get_manifest()

        async def get_drm_info(self):
            return signaling

    return WrapperWebAudio(codec=source.codec, origin=StreamOrigin.WRAPPER_HLS,
        url=source.url, transport=StreamTransport(), media_id=source.media_id,
        bitrate=source.bitrate, codec_string=source.codec_string)


def decryption_allowed(codec, method):
    return codec in SUPPORTED_SONG_CODECS and method in {"wrapper", "fixed_prefetch_key"}
