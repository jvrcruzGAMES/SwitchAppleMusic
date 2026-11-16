"""Bounded, ephemeral fMP4 cache; the console never receives wrapper secrets."""
import asyncio
import time
from pathlib import Path
from fastapi import HTTPException
from .hls import playlist
from .codecs import select_source, wrapper_source, decryption_allowed

class Playback:
    def __init__(self, settings, apple):
        self.s, self.apple = settings, apple
        self.active = {}
        self.touched = {}
        self.lock = asyncio.Lock()  # wrapper's account/session has one playback pipeline
        self.s.cache_dir.mkdir(parents=True, exist_ok=True)

    def prune(self, reserve=0):
        now = time.time()
        self.touched={p:t for p,t in self.touched.items() if p.exists()}
        files = sorted(self.s.cache_dir.glob("*.m4a"), key=lambda p: self.touched.get(p,p.stat().st_mtime))
        total = sum(p.stat().st_size for p in files)
        for p in files:
            size = p.stat().st_size
            if self.active.get(p, 0):
                continue
            if now - self.touched.get(p,p.stat().st_mtime) > self.s.cache_ttl or total + reserve > self.s.max_cache_bytes:
                p.unlink(missing_ok=True)
                p.with_suffix(".m3u8").unlink(missing_ok=True)
                self.touched.pop(p,None)
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
                self.touched[target]=time.time()
                if acquire:
                    self.active[target] = self.active.get(target, 0) + 1
                return target
            self.prune(self.s.max_track_bytes)
            # A regenerated rendition may have different byte offsets.
            target.with_suffix(".m3u8").unlink(missing_ok=True)
            temp = target.with_suffix(".part")
            try:
                async with asyncio.timeout(180):
                    await self.render(song_id, temp)
                temp.replace(target)
                self.touched[target]=time.time()
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

    async def manifest(self, song_id):
        path=await self.materialize(song_id,acquire=True)
        try:
            cached=path.with_suffix('.m3u8')
            if cached.exists():return cached.read_text()
            song=await self.apple.catalog_song('songs',song_id)
            text=await asyncio.to_thread(playlist,path,song_id,song.get('attributes',{}).get('durationInMillis',0)/1000)
            # Small synchronous write publishes only a complete playlist; no await
            # allows another request to interleave this rename.
            temporary=cached.with_suffix('.m3u8.part')
            try:
                temporary.write_text(text);temporary.replace(cached)
            finally:temporary.unlink(missing_ok=True)
            return text
        finally:self.release(path)

    async def render(self, song_id: str, target: Path):
        # Imports stay lazy so browsing and tests work without the Rust extension.
        try:
            from gamdl.api import AppleMusicApi, WrapperApi
            from gamdl.api.auth import credentials_from_wrapper
            from gamdl.transport import ApiHttpClient
            from gamdl.media import Song
            from gamdl.stream import Streamer
        except ImportError:
            raise HTTPException(503, "Install backend playback dependencies") from None
        resource = await self.apple.catalog_song("songs", song_id)
        async with (await WrapperApi.create(base_url=self.s.wrapper_url,
                decrypt_host=self.s.decrypt_host, decrypt_port=self.s.decrypt_port)) as wrapper:
            credentials = credentials_from_wrapper(wrapper.me)
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
        await self.fragment(target)

    async def fragment(self,target):
        # A second bounded host file is used only during a lossless codec-copy remux.
        self.prune(target.stat().st_size+self.s.max_track_bytes)
        output=target.with_suffix('.fragmented')
        process=None
        try:
            process=await asyncio.create_subprocess_exec(
                'ffmpeg','-hide_banner','-loglevel','error','-nostdin','-y','-protocol_whitelist','file','-i',str(target),
                '-map','0:a:0','-c:a','copy','-vn','-f','mp4',
                '-movflags','+empty_moov+default_base_moof+frag_keyframe+global_sidx',
                '-frag_duration','2000000','-fs',str(self.s.max_track_bytes),str(output),
                stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.DEVNULL)
            if await process.wait()!=0:raise HTTPException(502,'Audio fragmentation failed')
            if not output.exists() or not 0<output.stat().st_size<self.s.max_track_bytes:
                raise HTTPException(413,'Fragmented track exceeds configured cache limit')
            output.replace(target)
        except FileNotFoundError:
            raise HTTPException(503,'Install FFmpeg on the backend host') from None
        finally:
            if process is not None and process.returncode is None:
                process.kill();await process.wait()
            output.unlink(missing_ok=True)
