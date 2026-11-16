"""Bounded, ephemeral fMP4 cache; the console never receives wrapper secrets."""
import asyncio
import time
from pathlib import Path
from fastapi import HTTPException
from .codecs import select_source, wrapper_source, decryption_allowed

class Playback:
    def __init__(self, settings, apple):
        self.s, self.apple = settings, apple
        self.active = {}
        self.lock = asyncio.Lock()  # wrapper's account/session has one playback pipeline
        self.s.cache_dir.mkdir(parents=True, exist_ok=True)

    def prune(self, reserve=0):
        now = time.time()
        files = sorted(self.s.cache_dir.glob("*.m4a"), key=lambda p: p.stat().st_mtime)
        total = sum(p.stat().st_size for p in files)
        for p in files:
            size = p.stat().st_size
            if self.active.get(p, 0):
                continue
            if now - p.stat().st_mtime > self.s.cache_ttl or total + reserve > self.s.max_cache_bytes:
                p.unlink(missing_ok=True)
                total -= size
        if total + reserve > self.s.max_cache_bytes:
            raise HTTPException(503, "Audio cache is busy; retry after active playback requests finish")

    def release(self, path):
        self.active[path] = max(0, self.active.get(path, 0) - 1)
        if not self.active[path]:
            self.active.pop(path, None)

    async def materialize(self, song_id, acquire=False):
        async with self.lock:
            self.prune()
            target = self.s.cache_dir / (song_id + ".m4a")
            if target.exists():
                if acquire:
                    self.active[target] = self.active.get(target, 0) + 1
                return target
            self.prune(self.s.max_track_bytes)
            temp = target.with_suffix(".part")
            try:
                async with asyncio.timeout(180):
                    await self.render(song_id, temp)
                temp.replace(target)
                if acquire:
                    self.active[target] = self.active.get(target, 0) + 1
                return target
            except asyncio.CancelledError:
                raise
            except HTTPException:
                raise
            except Exception:
                raise HTTPException(502, "Wrapper stream preparation failed") from None
            finally:
                temp.unlink(missing_ok=True)

    async def render(self, song_id: str, target: Path):
        # Imports stay lazy so browsing and tests work without the Rust extension.
        try:
            from gamdl.api import AppleMusicApi, WrapperApi
            from gamdl.api.auth import AppleMusicCredentials
            from gamdl.transport import ApiHttpClient
            from gamdl.media import Song
            from gamdl.stream import Streamer
        except ImportError:
            raise HTTPException(503, "Install backend playback dependencies") from None
        resource = await self.apple.catalog_song("songs", song_id)
        credentials = AppleMusicCredentials(self.apple.developer_token(), await self.apple.user_token())
        async with (await WrapperApi.create(base_url=self.s.wrapper_url,
                decrypt_host=self.s.decrypt_host, decrypt_port=self.s.decrypt_port)) as wrapper:
            async with AppleMusicApi(ApiHttpClient("apple_music"), credentials,
                                     self.s.storefront, "en-US") as api:
                song = Song(song_id, api=api, metadata=resource)
                source = await select_source(song, wrapper)
                codec = source.codec.value
                source = await wrapper_source(source)
                async with Streamer(source, wrapper_api=wrapper, concurrency=2,
                                    chunk_size=64 * 1024) as stream:
                    if not decryption_allowed(codec, stream.decryption_method):
                        raise HTTPException(502, "No supported decrypted audio stream available")
                    size = 0
                    with target.open("wb") as output:
                        async for chunk in stream:
                            size += len(chunk)
                            if size > self.s.max_track_bytes:
                                raise HTTPException(413, "Track exceeds configured memory cache limit")
                            output.write(chunk)
