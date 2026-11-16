import json
import httpx
from fastapi import HTTPException

def parse_subtitles(body):
    rows = json.loads(body)
    lines = []
    for row in rows:
        stamp = row["time"]
        seconds = stamp.get("total")
        if seconds is None:
            seconds = stamp.get("minutes", 0) * 60 + stamp.get("seconds", 0) + stamp.get("hundredths", 0) / 100
        lines.append({"time_ms": max(0, round(float(seconds) * 1000)), "text": row["text"]})
    return sorted(lines, key=lambda row: row["time_ms"])

async def lyrics(settings, http, song):
    if not settings.musixmatch_key:
        raise HTTPException(503, "Configure a Musixmatch API key with synchronized subtitle access")
    a = song.get("attributes", {})
    params = {"apikey": settings.musixmatch_key, "format": "json", "subtitle_format": "mxm"}
    if a.get("isrc"):
        params["track_isrc"] = a["isrc"]
    else:
        params.update(q_track=a.get("name", ""), q_artist=a.get("artistName", ""))
    try:
        r = await http.get("https://api.musixmatch.com/ws/1.1/matcher.subtitle.get", params=params)
        r.raise_for_status()
        message = r.json()["message"]
        code = message["header"]["status_code"]
        if code != 200:
            raise HTTPException(404 if code == 404 else 502, "Musixmatch subtitles unavailable for this track or API plan")
        sub = message["body"]["subtitle"]
        return {"provider": "Musixmatch", "copyright": sub.get("lyrics_copyright", ""),
                "tracking_url": sub.get("pixel_tracking_url", ""),
                "lines": parse_subtitles(sub["subtitle_body"])}
    except (httpx.HTTPError, KeyError, ValueError, TypeError):
        raise HTTPException(502, "Musixmatch returned an invalid subtitle response") from None
