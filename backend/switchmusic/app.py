from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit
import asyncio
import base64
import binascii
import secrets
import re
import qrcode
import httpx
from fastapi import FastAPI, Depends, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse
from pydantic import BaseModel, ConfigDict, Field
from .settings import Settings
from .accounts import Accounts
from .apple import Apple, KINDS, normalize
from .playback import Playback
from .artwork import Artwork
from .lyrics import lyrics

COOKIE='switchmusic_session'
WEB=Path(__file__).parent/'web'

class Body(BaseModel):
    model_config=ConfigDict(extra='forbid')
class Login(Body):
    username: str=Field(min_length=1,max_length=32)
    password: str=Field(min_length=1,max_length=256)
class NewAccount(Login):pass
class AccountUpdate(Body):
    password: str|None=Field(None,min_length=12,max_length=256)
    disabled: bool|None=None
class PasswordChange(Body):
    old_password: str=Field(min_length=1,max_length=256)
    new_password: str=Field(min_length=12,max_length=256)
class AppleStart(Body):
    qr_code: str|None=Field(None,min_length=43,max_length=43,pattern=r'^[A-Za-z0-9_-]+$')
class AppleCallback(AppleStart):
    state: str=Field(min_length=1,max_length=256)
    music_user_token: str=Field(min_length=1,max_length=32768)

class QRCode(Body):
    code: str=Field(min_length=43,max_length=43,pattern=r'^[A-Za-z0-9_-]+$')

@dataclass
class Principal:
    user: dict
    session_token: str|None=None
    csrf: str|None=None

class LeasedAudioResponse(FileResponse):
    def __init__(self,path,playback):
        super().__init__(path,media_type='audio/mp4',headers={'Cache-Control':'private, no-store'})
        self.playback=playback
    async def __call__(self,scope,receive,send):
        try:await super().__call__(scope,receive,send)
        finally:self.playback.release(self.path)

def create_app(settings=None,transport=None):
    @asynccontextmanager
    async def lifespan(app):
        s=settings or Settings.from_env()
        app.state.settings=s
        app.state.accounts=await asyncio.to_thread(Accounts,s)
        async with httpx.AsyncClient(timeout=30,transport=transport,follow_redirects=False) as http:
            app.state.http=http
            app.state.apple=Apple(s,http)
            app.state.playback=Playback(s,app.state.apple)
            app.state.artwork=Artwork(http)
            yield

    app=FastAPI(title='SwitchMusic',version='0.2.0',lifespan=lifespan,
                docs_url=None,redoc_url=None,openapi_url=None)

    def check_origin(request):
        origin=request.headers.get('origin')
        expected=urlsplit(app.state.settings.public_url)
        if origin and origin != f'{expected.scheme}://{expected.netloc}':
            raise HTTPException(403,'Cross-origin request denied')

    @app.middleware('http')
    async def response_headers(request,call_next):
        response=await call_next(request)
        response.headers['X-Content-Type-Options']='nosniff'
        # MusicKit's cross-origin authorization popup needs the opener origin.
        # Send no path, query or QR capability in the referrer.
        response.headers['Referrer-Policy']='strict-origin'
        response.headers['Cache-Control']='private, no-store'
        return response

    async def current(request: Request):
        store=app.state.accounts
        authorization=request.headers.get('authorization','')
        if authorization:
            scheme,_,value=authorization.partition(' ')
            if scheme.lower()!='basic' or len(value)>2048:raise HTTPException(401,'Sign in with your account')
            try:
                username,password=base64.b64decode(value,validate=True).decode().split(':',1)
            except (ValueError,UnicodeError,binascii.Error):raise HTTPException(401,'Invalid account credentials') from None
            if len(username)>32 or len(password)>256:raise HTTPException(401,'Invalid account credentials')
            user=await asyncio.to_thread(store.authenticate,username,password,request.client.host if request.client else 'api')
            principal=Principal(user)
        else:
            token=request.cookies.get(COOKIE,'')
            if not token or len(token)>256:raise HTTPException(401,'Sign in required')
            user,csrf=store.session(token)
            principal=Principal(user,token,csrf)
        if request.method not in ('GET','HEAD','OPTIONS'):
            check_origin(request)
            if principal.session_token and not secrets.compare_digest(request.headers.get('x-csrf-token',''),principal.csrf):
                raise HTTPException(403,'Invalid CSRF token')
        return principal

    async def catalog_access(request: Request):
        # Anonymous catalog browsing needs only the server developer JWT.
        # Still reject invalid credentials when a caller explicitly supplies them.
        if request.headers.get('authorization') or request.cookies.get(COOKIE):
            await current(request)

    async def admin(principal: Principal=Depends(current)):
        if not principal.user['admin']:raise HTTPException(403,'Admin access required')
        return principal

    def apple(principal):
        user=principal.user
        return Apple(app.state.settings,app.state.http,app.state.accounts.apple_token(user),user['storefront'])

    def valid_id(value):
        if not re.fullmatch(r'[A-Za-z0-9.\-]{1,128}',value):raise HTTPException(400,'Invalid resource ID')
        return value

    @app.get('/')
    async def website():return FileResponse(WEB/'index.html',media_type='text/html')
    @app.get('/assets/app.js')
    async def javascript():return FileResponse(WEB/'app.js',media_type='text/javascript')
    @app.get('/assets/style.css')
    async def stylesheet():return FileResponse(WEB/'style.css',media_type='text/css')
    @app.get('/health')
    async def health():return {'status':'ok','version':'0.2.0'}

    @app.post('/auth/login')
    async def login(body: Login,request: Request,response: Response):
        check_origin(request)
        user=await asyncio.to_thread(app.state.accounts.authenticate,body.username,body.password,request.client.host if request.client else 'api')
        old=request.cookies.get(COOKIE)
        if old:app.state.accounts.logout(old)
        token,csrf=app.state.accounts.login(user)
        response.set_cookie(COOKIE,token,max_age=43200,httponly=True,secure=app.state.settings.secure_cookies,samesite='strict',path='/')
        return {'user':app.state.accounts.public(user),'csrf':csrf}

    @app.get('/auth/me')
    async def me(principal: Principal=Depends(current)):
        return {'user':app.state.accounts.public(principal.user),'csrf':principal.csrf}

    @app.post('/auth/logout',status_code=204)
    async def logout(response: Response,principal: Principal=Depends(current)):
        if principal.session_token:app.state.accounts.logout(principal.session_token)
        response.delete_cookie(COOKIE,path='/')

    @app.post('/auth/password',status_code=204)
    async def password(body: PasswordChange,principal: Principal=Depends(current)):
        await asyncio.to_thread(app.state.accounts.authenticate,principal.user['username'],body.old_password)
        await asyncio.to_thread(app.state.accounts.change_password,principal.user['id'],body.new_password)

    @app.get('/admin/accounts')
    async def list_accounts(principal: Principal=Depends(admin)):
        return {'accounts':app.state.accounts.list()}

    @app.post('/admin/accounts',status_code=201)
    async def create_account(body: NewAccount,principal: Principal=Depends(admin)):
        return await asyncio.to_thread(app.state.accounts.create,body.username,body.password)

    @app.patch('/admin/accounts/{user_id}')
    async def update_account(user_id: int,body: AccountUpdate,principal: Principal=Depends(admin)):
        await asyncio.to_thread(app.state.accounts.manage,user_id,body.password,body.disabled)
        return {'ok':True}

    @app.delete('/admin/accounts/{user_id}',status_code=204)
    async def delete_account(user_id: int,principal: Principal=Depends(admin)):
        app.state.accounts.manage(user_id,delete=True)

    @app.delete('/admin/accounts/{user_id}/apple',status_code=204)
    async def admin_unlink(user_id: int,principal: Principal=Depends(admin)):
        app.state.accounts.unlink(user_id)

    def browser_session(principal):
        if not principal.session_token:raise HTTPException(400,'Sign in with a session to use Apple Music QR linking')

    @app.post('/auth/apple/qr/start')
    async def start_qr(principal: Principal=Depends(current)):
        browser_session(principal)
        origin=urlsplit(app.state.settings.public_url)
        if origin.scheme not in ('http','https') or not origin.netloc or origin.username or origin.password or origin.path not in ('','/') or origin.query or origin.fragment or len(origin.netloc)>300:
            raise HTTPException(503,'Set PUBLIC_URL to your phone-accessible server origin')
        code,confirmation=app.state.accounts.start_qr(principal.user['id'],principal.session_token)
        url=app.state.settings.public_url.rstrip('/')+'/#apple-qr='+code
        def matrix():
            image=qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M,border=4)
            image.add_data(url);image.make(fit=True)
            return [''.join('1' if cell else '0' for cell in row) for row in image.get_matrix()]
        return {'code':code,'confirmation':confirmation,'url':url,'expires_in':300,'matrix':await asyncio.to_thread(matrix)}

    @app.post('/auth/apple/qr/details')
    async def qr_details(body: QRCode,principal: Principal=Depends(current)):
        browser_session(principal)
        return app.state.accounts.qr(principal.user['id'],body.code)

    @app.post('/auth/apple/qr/status')
    async def qr_status(body: QRCode,principal: Principal=Depends(current)):
        browser_session(principal)
        return app.state.accounts.qr(principal.user['id'],body.code,principal.session_token,'status')

    @app.post('/auth/apple/qr/cancel',status_code=204)
    async def qr_cancel(body: QRCode,principal: Principal=Depends(current)):
        browser_session(principal)
        app.state.accounts.qr(principal.user['id'],body.code,principal.session_token,'cancel')

    @app.post('/auth/apple/start')
    async def start_apple(body: AppleStart|None=None,principal: Principal=Depends(current)):
        if not principal.session_token:raise HTTPException(400,'Sign in on the website to link Apple Music')
        authorization=await app.state.apple.authorization_token()
        return {'state':app.state.accounts.start_link(principal.user['id'],principal.session_token,body.qr_code if body else None),
                **authorization}

    @app.post('/auth/apple/callback')
    async def finish_apple(body: AppleCallback,principal: Principal=Depends(current)):
        if not principal.session_token:raise HTTPException(400,'Sign in on the website to link Apple Music')
        if body.music_user_token.strip()!=body.music_user_token or body.music_user_token.lower() in ('undefined','null') or '\r' in body.music_user_token or '\n' in body.music_user_token:
            raise HTTPException(422,'MusicKit did not return a valid Music User Token; complete Apple Music authorization before linking')
        app.state.accounts.consume_link(principal.user['id'],principal.session_token,body.state,body.qr_code)
        linked=Apple(app.state.settings,app.state.http,body.music_user_token)
        payload=await linked.get('me/storefront')
        data=payload.get('data',[])
        storefront=data[0].get('id','') if data else ''
        if not re.fullmatch('[a-z]{2}',storefront):raise HTTPException(502,'Apple did not return a valid storefront')
        app.state.accounts.link(principal.user['id'],body.music_user_token,storefront,body.qr_code)
        return {'user':app.state.accounts.public(app.state.accounts.get(principal.user['id']))}

    @app.delete('/auth/apple',status_code=204)
    async def unlink_apple(principal: Principal=Depends(current)):
        app.state.accounts.unlink(principal.user['id'])

    @app.get('/v1/library/{kind}')
    async def library(kind: str,offset: int=Query(0,ge=0,le=1000000),principal: Principal=Depends(current)):
        if kind not in KINDS:raise HTTPException(404,'Unknown library collection')
        return await apple(principal).page(kind,offset)

    @app.get('/v1/library/{kind}/{resource_id}/tracks')
    async def tracks(kind: str,resource_id: str,offset: int=Query(0,ge=0,le=1000000),principal: Principal=Depends(current)):
        if kind not in ('albums','playlists'):raise HTTPException(404,'Unknown track collection')
        return await apple(principal).page(kind,offset,valid_id(resource_id))

    @app.get('/v1/catalog/search')
    async def search(term: str=Query(min_length=1,max_length=200),offset: int=Query(0,ge=0,le=1000000),access=Depends(catalog_access)):
        return await app.state.apple.search(term,offset)

    @app.get('/v1/catalog/{kind}/{resource_id}/tracks')
    async def catalog_tracks(kind: str,resource_id: str,offset: int=Query(0,ge=0,le=1000000),access=Depends(catalog_access)):
        if kind not in ('albums','playlists'):raise HTTPException(404,'Unknown track collection')
        return await app.state.apple.catalog_page(kind,valid_id(resource_id),offset)

    @app.get('/v1/artwork/{kind}/{resource_id}')
    async def artwork(kind: str,resource_id: str,request: Request,size: int=Query(64),format: str=Query('png')):
        if size not in (64,256) or format not in ('png','rgba4444'):
            raise HTTPException(400,'Unsupported artwork variant')
        resource_id=valid_id(resource_id)
        if kind in ('songs','albums','playlists'):
            client=app.state.apple;scope='catalog'
            path='catalog/'+client.storefront+'/'+kind+'/'+resource_id
        elif kind in ('library-songs','library-albums','library-playlists'):
            principal=await current(request);client=apple(principal);await client.user_token()
            scope=str(principal.user['id']);path='me/library/'+kind.removeprefix('library-')+'/'+resource_id
        else:raise HTTPException(404,'Unknown artwork resource')
        async def load():
            response=await client.get(path);data=response.get('data',[])
            if not data:raise HTTPException(404,'Artwork resource unavailable')
            return data[0]
        data=await app.state.artwork.get((scope,kind,resource_id),load,size,format)
        return Response(data,media_type='image/png' if format=='png' else 'application/octet-stream')

    @app.get('/v1/resolve/{kind}/{resource_id}')
    async def resolve(kind: str,resource_id: str,principal: Principal=Depends(current)):
        if kind not in ('songs','library-songs'):raise HTTPException(422,'Only songs are playable')
        song=await apple(principal).catalog_song(kind,valid_id(resource_id))
        result=normalize(song);result['stream_path']='/v1/stream/'+song['id'];result['playback_path']='/v1/playback/'+song['id']+'.m3u8'
        return result

    @app.get('/v1/playback/{song_id}.m3u8')
    async def manifest(song_id: str):
        if not re.fullmatch(r'[0-9]{1,20}',song_id):raise HTTPException(400,'Expected numeric catalog ID')
        return Response(await app.state.playback.manifest(song_id),media_type='application/vnd.apple.mpegurl')

    @app.get('/v1/stream/{song_id}')
    async def stream(song_id: str):
        if not re.fullmatch(r'[0-9]{1,20}',song_id):raise HTTPException(400,'Expected numeric catalog ID')
        # Only canonical catalog audio is cached/shared; library access is resolved per user.
        path=await app.state.playback.materialize(song_id,acquire=True)
        return LeasedAudioResponse(path,app.state.playback)

    @app.get('/v1/lyrics/{song_id}')
    async def subtitles(song_id: str,principal: Principal=Depends(current)):
        song=await apple(principal).catalog_song('songs',valid_id(song_id))
        return await lyrics(song, app.state.settings)
    return app

app=create_app()
