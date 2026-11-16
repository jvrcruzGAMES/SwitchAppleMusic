from contextlib import asynccontextmanager
import secrets
import re
import httpx
from fastapi import FastAPI, Depends, Header, HTTPException, Query
from fastapi.responses import FileResponse
from .settings import Settings
from .apple import Apple, KINDS, normalize
from .playback import Playback
from .lyrics import lyrics

class LeasedAudioResponse(FileResponse):
    def __init__(self, path, playback):
        super().__init__(path, media_type="audio/mp4", headers={"Cache-Control": "private, no-store"})
        self.playback = playback

    async def __call__(self, scope, receive, send):
        try:
            await super().__call__(scope, receive, send)
        finally:
            self.playback.release(self.path)

def create_app(settings=None, transport=None):
    @asynccontextmanager
    async def lifespan(app):
        s = settings or Settings.from_env()
        async with httpx.AsyncClient(timeout=30, transport=transport, follow_redirects=False) as http:
            app.state.settings, app.state.http = s, http
            app.state.apple = Apple(s, http)
            app.state.playback = Playback(s, app.state.apple)
            yield

    app = FastAPI(title="SwitchMusic", version="0.1.0", lifespan=lifespan,
                  docs_url=None, redoc_url=None, openapi_url=None)

    async def auth(authorization: str = Header(default="")):
        if not secrets.compare_digest(authorization, "Bearer " + app.state.settings.device_token):
            raise HTTPException(401, "Invalid device token")

    def valid_id(value):
        if not re.fullmatch(r"[A-Za-z0-9.\-]{1,128}", value):
            raise HTTPException(400, "Invalid resource ID")
        return value

    @app.get("/health")
    async def health():
        return {"status": "ok", "version": "0.1.0"}

    @app.get("/v1/library/{kind}", dependencies=[Depends(auth)])
    async def library(kind: str, offset: int = Query(0, ge=0, le=1000000)):
        if kind not in KINDS:
            raise HTTPException(404, "Unknown library collection")
        return await app.state.apple.page(kind, offset)

    @app.get("/v1/library/{kind}/{resource_id}/tracks", dependencies=[Depends(auth)])
    async def tracks(kind: str, resource_id: str, offset: int = Query(0, ge=0, le=1000000)):
        if kind not in ("albums", "playlists"):
            raise HTTPException(404, "Unknown track collection")
        return await app.state.apple.page(kind, offset, valid_id(resource_id))

    @app.get("/v1/resolve/{kind}/{resource_id}", dependencies=[Depends(auth)])
    async def resolve(kind: str, resource_id: str):
        if kind not in ("songs", "library-songs"):
            raise HTTPException(422, "Only songs are playable")
        song = await app.state.apple.catalog_song(kind, valid_id(resource_id))
        result = normalize(song)
        result["stream_path"] = "/v1/stream/" + song["id"]
        return result

    @app.get("/v1/stream/{song_id}", dependencies=[Depends(auth)])
    async def stream(song_id: str):
        if not re.fullmatch(r"[0-9]{1,20}", song_id):
            raise HTTPException(400, "Expected numeric catalog ID")
        path = await app.state.playback.materialize(song_id, acquire=True)
        return LeasedAudioResponse(path, app.state.playback)

    @app.get("/v1/lyrics/{song_id}", dependencies=[Depends(auth)])
    async def subtitles(song_id: str):
        song = await app.state.apple.catalog_song("songs", valid_id(song_id))
        return await lyrics(app.state.settings, app.state.http, song)
    return app

app = create_app()
