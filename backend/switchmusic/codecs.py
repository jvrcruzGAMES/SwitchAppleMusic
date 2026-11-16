"""The console's complete codec policy; delivery names are not decoder names."""
from fastapi import HTTPException

CODEC_PRIORITY = ("alac", "aac", "aac-web")
SUPPORTED_SONG_CODECS = frozenset({"alac", "alac-lossless", "alac-hires", "aac", "aac-web"})

async def select_source(song, wrapper):
    from gamdl.media import SongCodec
    source = await song.get_stream(
        codec_priority=tuple(SongCodec(codec) for codec in CODEC_PRIORITY),
        wrapper_api=wrapper,
    )
    codec = source.codec.value
    if codec not in SUPPORTED_SONG_CODECS:
        raise HTTPException(502, "Selected codec is unsupported by the console")
    return source


def decryption_allowed(codec, method):
    # Web AAC is a distinct Widevine delivery path, even with wrapper connected.
    if codec == "aac-web":
        return method in {"widevine", "none"}
    return codec in SUPPORTED_SONG_CODECS and method in {"wrapper", "fixed_prefetch_key"}
