import time
import httpx
import jwt
from fastapi import HTTPException

KINDS = {"songs": "library-songs", "albums": "library-albums", "playlists": "library-playlists"}

def normalize(resource):
    a = resource.get("attributes", {})
    return {"id": resource["id"], "type": resource["type"],
            "title": a.get("name", "Untitled"), "artist": a.get("artistName", ""),
            "album": a.get("albumName", ""), "duration_ms": a.get("durationInMillis", 0),
            "catalog_id": a.get("playParams", {}).get("catalogId", ""),
            "isrc": a.get("isrc", "")}

class Apple:
    def __init__(self, settings, http, user_token=None, storefront=None):
        self.s, self.http = settings, http
        self.token = user_token
        self.storefront = storefront or settings.storefront
        self._jwt, self._until = "", 0

    def developer_token(self):
        now = int(time.time())
        if now >= self._until:
            self._jwt = jwt.encode({"iss": self.s.team_id, "iat": now, "exp": now + 3600},
                self.s.private_key.read_text(), algorithm="ES256", headers={"kid": self.s.key_id})
            self._until = now + 3300
        return self._jwt

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
                raise HTTPException(r.status_code, "Apple Music authorization expired or denied")
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
