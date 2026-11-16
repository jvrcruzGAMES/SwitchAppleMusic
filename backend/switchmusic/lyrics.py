"""Keyless synchronized lyrics via syncedlyrics, normalized for the console."""
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


async def lyrics(song):
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
