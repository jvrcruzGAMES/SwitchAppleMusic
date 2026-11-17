"""Apple Music/Gamdl first, then keyless syncedlyrics; shared console timing."""
import asyncio
import re

import syncedlyrics
from fastapi import HTTPException

TIMESTAMP = re.compile(r'\[(\d+):([0-5]\d)(?:[.:](\d+))?\]')
OFFSET = re.compile(r'\[offset:([+-]?\d+)\]', re.IGNORECASE)
WORD_TIMESTAMP = re.compile(r'<\d+:[0-5]\d(?:\.\d+)?>')


def parse_lrc(body):
    offsets = OFFSET.findall(body)
    offset = int(offsets[-1]) if offsets else 0
    lines = []
    for raw in body.splitlines():
        raw = raw.strip().lstrip('\ufeff')
        if not TIMESTAMP.match(raw):
            continue
        stamps = []
        end = 0
        while match := TIMESTAMP.match(raw, end):
            stamps.append(match)
            end = match.end()
        text = WORD_TIMESTAMP.sub('', raw[end:]).strip()
        for stamp in stamps:
            minutes, seconds, fraction = stamp.groups()
            millis = int(((fraction or '') + '000')[:3])
            time_ms = (int(minutes) * 60 + int(seconds)) * 1000 + millis + offset
            lines.append({'time_ms': max(0, time_ms), 'text': text})
    return sorted(lines, key=lambda row: row['time_ms'])


async def apple_lrc(song, settings):
    """Use the pinned Gamdl Song API for Apple TTML -> standard LRC.

    Wrapper credentials remain server-side. No audio/DRM pipeline is opened.
    Imports are lazy so syncedlyrics still works without playback extras.
    """
    if settings is None or not song.get('id'):
        return None
    from gamdl.api import AppleMusicApi, WrapperApi
    from gamdl.api.auth import credentials_from_wrapper
    from gamdl.transport import ApiHttpClient
    from gamdl.media import Song

    async with asyncio.timeout(10):
        async with (await WrapperApi.create(base_url=settings.wrapper_url,
                decrypt_host=settings.decrypt_host, decrypt_port=settings.decrypt_port)) as wrapper:
            credentials = credentials_from_wrapper(wrapper.me)
            async with AppleMusicApi(ApiHttpClient('apple_music'), credentials,
                                     settings.storefront, 'en-US') as api:
                document = await Song(song['id'], api=api, metadata=song).get_lyrics()
                if document is not None and document.is_synced:
                    return document.to_lrc(enhanced=False)
    return None


async def lyrics(song, settings=None):
    try:
        body = await apple_lrc(song, settings)
        lines = parse_lrc(body) if isinstance(body, str) else []
        if any(line['text'] for line in lines):
            return {'provider': 'apple-music', 'copyright': '', 'tracking_url': '', 'lines': lines}
    except Exception:
        # Missing Gamdl, wrapper/API failures, malformed TTML and timeouts all
        # fall back. Cancellation is deliberately not swallowed.
        pass
    attributes = song.get('attributes', {})
    title = attributes.get('name', '').strip()
    artist = attributes.get('artistName', '').strip()
    if not title:
        raise HTTPException(422, 'Song title is required to find synchronized lyrics')
    query = ' '.join(part for part in (title, artist) if part)
    try:
        # The package performs blocking requests; keep them off the API event loop.
        body = await asyncio.to_thread(syncedlyrics.search, query, synced_only=True)
    except Exception:
        raise HTTPException(502, 'Synchronized lyric search failed') from None
    if not isinstance(body, str) or not body.strip():
        raise HTTPException(404, 'No synchronized lyrics available for this track')
    lines = parse_lrc(body)
    if not lines:
        raise HTTPException(404, 'No synchronized lyrics available for this track')
    return {'provider': 'syncedlyrics', 'copyright': '', 'tracking_url': '', 'lines': lines}
