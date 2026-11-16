"""Small, cached Apple artwork variants; no client connects to Apple's CDN."""
import asyncio
from collections import OrderedDict
from io import BytesIO
import time
from urllib.parse import urlsplit
import warnings
import httpx
from fastapi import HTTPException
from PIL import Image, ImageOps, UnidentifiedImageError

class Artwork:
    def __init__(self,http):
        self.http=http;self.cache=OrderedDict();self.urls=OrderedDict();self.bytes=0
        self.lock=asyncio.Lock()

    async def get(self,key,load,size,format):
        async with self.lock:
            now=time.monotonic();variant=(key,size,format)
            cached=self.cache.get(variant)
            if cached and cached[0]>now:
                self.cache.move_to_end(variant);return cached[1]
            source=self.urls.get(key)
            if not source or source[0]<=now:
                resource=await load();url=resource.get('attributes',{}).get('artwork',{}).get('url','')
                try:parsed=urlsplit(url);port=parsed.port
                except ValueError:raise HTTPException(404,'No Apple Music artwork available') from None
                if parsed.scheme!='https' or not parsed.hostname or not parsed.hostname.endswith('.mzstatic.com') or parsed.username or parsed.password or port not in (None,443):
                    raise HTTPException(404,'No Apple Music artwork available')
                self.urls[key]=(now+600,url);self.urls.move_to_end(key)
                while len(self.urls)>256:self.urls.popitem(last=False)
            else:url=source[1]
            url=url.replace('{w}',str(size)).replace('{h}',str(size)).replace('{f}','jpg')
            raw=bytearray()
            try:
                # Explicitly send no Apple developer or Music User Token to the CDN.
                async with self.http.stream('GET',url,timeout=8) as response:
                    response.raise_for_status()
                    async for block in response.aiter_bytes():
                        if len(raw)+len(block)>2*1024*1024:raise HTTPException(502,'Artwork image too large')
                        raw.extend(block)
                result=await asyncio.to_thread(convert,bytes(raw),size,format)
            except (httpx.HTTPError,ValueError,OSError,UnidentifiedImageError,Image.DecompressionBombError,Image.DecompressionBombWarning):
                raise HTTPException(502,'Artwork unavailable') from None
            previous=self.cache.pop(variant,None)
            if previous:self.bytes-=len(previous[1])
            self.cache[variant]=(now+600,result);self.bytes+=len(result)
            while self.bytes>8*1024*1024 or len(self.cache)>256:
                _,(_,old)=self.cache.popitem(last=False);self.bytes-=len(old)
            return result

def convert(raw,size,format):
    with warnings.catch_warnings():
        warnings.simplefilter('error',Image.DecompressionBombWarning)
        with Image.open(BytesIO(raw)) as source:
            if source.width>2048 or source.height>2048:raise ValueError('Artwork dimensions')
            image=ImageOps.fit(source.convert('RGB'),(size,size),method=Image.Resampling.LANCZOS)
    if format=='rgba4444':
        # libultrahand drawBitmapRGBA4444 expects bytes RG, BA (high nibble first).
        pixels=image.tobytes();output=bytearray(size*size*2)
        for index in range(size*size):
            r,g,b=pixels[index*3:index*3+3]
            output[index*2]=(r&0xf0)|(g>>4);output[index*2+1]=(b&0xf0)|0xf
        return bytes(output)
    output=BytesIO();image.save(output,format='PNG');return output.getvalue()
