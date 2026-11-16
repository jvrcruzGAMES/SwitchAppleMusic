import time
import re
from urllib.parse import urlsplit
from cryptography.exceptions import UnsupportedAlgorithm
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
import httpx
import jwt
from fastapi import HTTPException

KINDS = {"songs": "library-songs", "albums": "library-albums", "playlists": "library-playlists"}

def normalize(resource):
    a = resource.get("attributes", {})
    return {"id": resource["id"], "type": resource["type"],
            "artwork_path": "/v1/artwork/"+resource["type"]+"/"+resource["id"] if a.get("artwork",{}).get("url") else "",
            "title": a.get("name", "Untitled"), "artist": a.get("artistName", ""),
            "album": a.get("albumName", ""), "duration_ms": a.get("durationInMillis", 0),
            "catalog_id": a.get("playParams", {}).get("catalogId", ""),
            "isrc": a.get("isrc", "")}

class Apple:
    def __init__(self, settings, http, user_token=None, storefront=None):
        self.s, self.http = settings, http
        self.token = user_token
        self.storefront = storefront or settings.storefront
        self._tokens = {}
        self._verified_token, self._verified_until = "", 0

    def developer_token(self,origin=None):
        now=int(time.time());cached=self._tokens.get(origin)
        if cached and now<cached[1]:return cached[0]
        team=self.s.team_id.strip();key_id=self.s.key_id.strip()
        if not re.fullmatch('[A-Z0-9]{10}',team) or not re.fullmatch('[A-Z0-9]{10}',key_id):
            raise HTTPException(503,'Set APPLE_TEAM_ID and APPLE_KEY_ID to the 10-character identifiers for your MusicKit key')
        try:
            key=serialization.load_pem_private_key(self.s.private_key.read_bytes(),password=None)
            if not isinstance(key,ec.EllipticCurvePrivateKey) or not isinstance(key.curve,ec.SECP256R1):raise ValueError('Expected P-256')
            claims={'iss':team,'iat':now,'exp':now+3600}
            if origin:claims['origin']=[origin]
            token=jwt.encode(claims,key,algorithm='ES256',headers={'kid':key_id})
        except (OSError,ValueError,TypeError,UnsupportedAlgorithm,jwt.PyJWTError):
            raise HTTPException(503,'Cannot load the MusicKit signing key; check APPLE_KEY_FILE permissions and use an unencrypted P-256 .p8 MusicKit key') from None
        self._tokens[origin]=(token,now+3300)
        return token

    async def authorization_token(self):
        public=urlsplit(self.s.public_url)
        if public.scheme not in ('https','http') or not public.netloc or public.username or public.password or public.path not in ('','/') or public.query or public.fragment:
            raise HTTPException(503,'Set PUBLIC_URL to the exact origin used to open this website')
        origin=f'{public.scheme}://{public.netloc}'
        token=self.developer_token(origin)
        if self._verified_token!=token or time.time()>=self._verified_until:
            try:
                response=await self.http.get('https://api.music.apple.com/v1/catalog/'+self.s.storefront+'/search',
                    params={'term':'music','types':'songs','limit':1},headers={'Authorization':'Bearer '+token,'Origin':origin})
            except httpx.HTTPError:
                raise HTTPException(502,'Could not verify the MusicKit developer JWT with Apple; check server connectivity and retry') from None
            if response.status_code in (401,403):
                raise HTTPException(503,f'Apple rejected the MusicKit developer JWT (HTTP {response.status_code}); check that APPLE_TEAM_ID, APPLE_KEY_ID and APPLE_KEY_FILE belong to the same active MusicKit key and that the server clock is correct')
            if response.status_code!=200:
                raise HTTPException(502,f'Apple developer-token verification returned HTTP {response.status_code}; check APPLE_STOREFRONT and retry')
            self._verified_token=token;self._verified_until=time.time()+120
        return {'developer_token':token,'developer_token_verified':True,
                'expires_at':jwt.decode(token,options={'verify_signature':False})['exp']}

    async def user_token(self):
        if not self.token:
            raise HTTPException(409, "Link your Apple Music library on the website first")
        return self.token

    async def get(self, path, **params):
        headers = {"Authorization": "Bearer " + self.developer_token()}
        if path.startswith("me/"):
            headers["Music-User-Token"] = await self.user_token()
        try:
            r = await self.http.get("https://api.music.apple.com/v1/" + path,
                                    params=params, headers=headers)
            if r.status_code in (401, 403):
                if path.startswith("me/"):
                    raise HTTPException(409, "Apple Music authorization expired or denied; relink your library on the website")
                raise HTTPException(502, "Apple Music server authorization failed")
            if r.status_code == 404:
                raise HTTPException(404, "Apple Music resource unavailable")
            r.raise_for_status()
            return r.json()
        except (httpx.HTTPError, ValueError):
            raise HTTPException(502, "Apple Music request failed") from None

    async def page(self, kind, offset, parent_id=None):
        path = "me/library/" + kind
        if parent_id:
            path += "/" + parent_id + "/tracks"
        payload = await self.get(path, limit=100, offset=offset)
        data = payload.get("data", [])
        return {"items": [normalize(x) for x in data],
                "next_offset": offset + len(data) if payload.get("next") and data else None}

    async def catalog_song(self, kind, song_id):
        if kind == "library-songs":
            p = await self.get("me/library/songs/" + song_id + "/catalog")
        else:
            p = await self.get("catalog/" + self.storefront + "/songs/" + song_id)
        data = p.get("data", [])
        if not data:
            raise HTTPException(422, "This library upload has no playable catalog match")
        return data[0]

    async def catalog_page(self, kind, resource_id, offset):
        payload=await self.get("catalog/"+self.storefront+"/"+kind+"/"+resource_id+"/tracks",limit=100,offset=offset)
        data=payload.get("data",[])
        return {"items":[normalize(x) for x in data],"next_offset":offset+len(data) if payload.get("next") and data else None}

    async def search(self,term,offset):
        payload=await self.get("catalog/"+self.storefront+"/search",term=term,types="songs,albums,playlists",limit=25,offset=offset)
        results=payload.get("results",{})
        return {"items":[normalize(x) for kind in ("songs","albums","playlists") for x in results.get(kind,{}).get("data",[])],
                "next_offset":offset+25 if any(results.get(kind,{}).get("next") for kind in results) else None}
