import base64
from dataclasses import replace
from pathlib import Path
import httpx
import pytest
from fastapi.testclient import TestClient
from switchmusic.app import create_app, COOKIE
from test_backend import settings, PASSWORD, HEADERS

USER_PASSWORD='Member-password-12345'

def basic(username,password=USER_PASSWORD):
    return {'Authorization':'Basic '+base64.b64encode((username+':'+password).encode()).decode()}

@pytest.fixture
def website(settings):
    def upstream(request):
        if request.url.host!='api.music.apple.com':raise AssertionError(str(request.url))
        if '/me/' in request.url.path:
            token=request.headers['Music-User-Token']
            if token=='invalid':return httpx.Response(403,json={})
            if request.url.path.endswith('/storefront'):return httpx.Response(200,json={'data':[{'id':'us'}]})
            return httpx.Response(200,json={'data':[{'id':'i.track','type':'library-songs','attributes':{'name':token,'artistName':'Artist'}}]})
        assert 'Music-User-Token' not in request.headers
        if request.url.path.endswith('/search'):
            return httpx.Response(200,json={'results':{'songs':{'data':[{'id':'123','type':'songs','attributes':{'name':'Public song'}}]}}})
        return httpx.Response(200,json={'data':[{'id':'123','type':'songs','attributes':{'name':'Public song'}}]})
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:yield client

def create(client,username='alice'):
    response=client.post('/admin/accounts',headers=HEADERS,json={'username':username,'password':USER_PASSWORD})
    assert response.status_code==201,response.text
    return response.json()['id']

def login(client,username='alice',password=USER_PASSWORD):
    response=client.post('/auth/login',json={'username':username,'password':password},headers={'Origin':'http://testserver'})
    assert response.status_code==200,response.text
    return {'X-CSRF-Token':response.json()['csrf'],'Origin':'http://testserver'}

def link(client,token):
    headers=login(client)
    start=client.post('/auth/apple/start',headers=headers)
    assert start.status_code==200,start.text
    flow=start.json()
    callback=client.post('/auth/apple/callback',headers=headers,json={'state':flow['state'],'music_user_token':token})
    assert callback.status_code==200,callback.text
    return headers

def test_only_admin_creates_accounts(website):
    assert website.post('/admin/accounts',json={'username':'alice','password':USER_PASSWORD}).status_code==401
    create(website)
    assert website.post('/admin/accounts',headers=basic('alice'),json={'username':'bob','password':USER_PASSWORD}).status_code==403
    assert website.post('/admin/accounts',headers=HEADERS,json={'username':'bob','password':USER_PASSWORD,'admin':True}).status_code==422
    assert website.post('/auth/register',json={}).status_code==404
    assert website.post('/admin/accounts',headers=HEADERS,json={'username':'alice','password':USER_PASSWORD}).status_code==409

def test_cookie_csrf_origin_and_logout(website):
    create(website)
    headers=login(website)
    cookie=website.cookies.get(COOKIE)
    assert website.get('/auth/me').json()['user']['username']=='alice'
    assert website.post('/auth/logout').status_code==403
    assert website.post('/auth/logout',headers={**headers,'Origin':'https://other.example'}).status_code==403
    assert website.post('/auth/logout',headers=headers).status_code==204
    assert website.get('/auth/me',headers={'Cookie':COOKIE+'='+cookie}).status_code==401
    assert website.post('/auth/login',headers={'Origin':'https://other.example'},json={'username':'admin','password':PASSWORD}).status_code==403

def test_public_catalog_does_not_require_an_apple_link(website):
    create(website)
    assert website.get('/v1/catalog/search?term=test',headers=basic('alice')).json()['items'][0]['title']=='Public song'
    assert website.get('/v1/resolve/songs/123',headers=basic('alice')).status_code==200
    assert website.get('/v1/library/songs',headers=basic('alice')).status_code==409
    assert website.get('/v1/catalog/search?term=test',headers={'Authorization':'Bearer old-device-token'}).status_code==401

def test_apple_links_are_isolated_encrypted_and_unlinkable(website,settings):
    alice=create(website)
    create(website,'bob')
    headers=link(website,'ALICE_LIBRARY_SECRET')
    assert website.get('/v1/library/songs').json()['items'][0]['title']=='ALICE_LIBRARY_SECRET'
    assert website.get('/v1/library/songs',headers=basic('bob')).status_code==409
    assert 'ALICE_LIBRARY_SECRET' not in website.get('/auth/me').text
    assert b'ALICE_LIBRARY_SECRET' not in (settings.data_dir/'accounts.sqlite3').read_bytes()
    listing=website.get('/admin/accounts',headers=HEADERS).json()['accounts']
    assert next(x for x in listing if x['id']==alice)['apple_linked']
    assert website.delete('/auth/apple',headers=headers).status_code==204
    assert website.get('/v1/library/songs').status_code==409

def test_link_state_bound_to_user_and_session_and_consumed_once(website):
    create(website);create(website,'bob')
    alice=login(website)
    state=website.post('/auth/apple/start',headers=alice).json()['state']
    bob=login(website,'bob')
    body={'state':state,'music_user_token':'BOB_SECRET'}
    assert website.post('/auth/apple/callback',headers=bob,json=body).status_code==400
    # A replacement Alice login cannot consume the old session's state either.
    alice=login(website)
    assert website.post('/auth/apple/callback',headers=alice,json=body).status_code==400
    state=website.post('/auth/apple/start',headers=alice).json()['state']
    body={'state':state,'music_user_token':'ALICE_SECRET'}
    assert website.post('/auth/apple/callback',headers=alice,json=body).status_code==200
    assert website.post('/auth/apple/callback',headers=alice,json=body).status_code==400

def test_invalid_apple_token_does_not_create_a_link(website):
    create(website);headers=login(website)
    state=website.post('/auth/apple/start',headers=headers).json()['state']
    assert website.post('/auth/apple/callback',headers=headers,json={'state':state,'music_user_token':'invalid'}).status_code==409
    assert not website.get('/auth/me').json()['user']['apple_linked']

def test_admin_reset_disable_delete_revoke_access(website):
    uid=create(website);login(website)
    cookie=website.cookies.get(COOKIE)
    assert website.patch('/admin/accounts/'+str(uid),headers=HEADERS,json={'password':'Replacement-password-123'}).status_code==200
    assert website.get('/auth/me',headers={'Cookie':COOKIE+'='+cookie}).status_code==401
    assert website.get('/auth/me',headers=basic('alice')).status_code==401
    assert website.get('/auth/me',headers=basic('alice','Replacement-password-123')).status_code==200
    assert website.patch('/admin/accounts/'+str(uid),headers=HEADERS,json={'disabled':True}).status_code==200
    assert website.get('/auth/me',headers=basic('alice','Replacement-password-123')).status_code==401
    assert website.patch('/admin/accounts/'+str(uid),headers=HEADERS,json={'disabled':False}).status_code==200
    assert website.delete('/admin/accounts/'+str(uid),headers=HEADERS).status_code==204
    assert website.get('/auth/me',headers=basic('alice','Replacement-password-123')).status_code==401
    assert website.delete('/admin/accounts/1',headers=HEADERS).status_code==403
    assert website.patch('/admin/accounts/1',headers=HEADERS,json={'disabled':True}).status_code==403

def test_account_can_change_own_password(website):
    create(website);headers=login(website)
    assert website.post('/auth/password',headers=headers,json={'old_password':USER_PASSWORD,'new_password':'Changed-password-456'}).status_code==204
    assert website.get('/auth/me').status_code==401
    assert website.get('/auth/me',headers=basic('alice','Changed-password-456')).status_code==200

def test_bootstrap_does_not_reset_existing_admin_and_links_survive_restart(website,settings):
    create(website);link(website,'PERSISTENT_LIBRARY')
    from switchmusic.accounts import Accounts
    restarted=Accounts(replace(settings,admin_password='A different startup password'))
    assert restarted.authenticate('admin',PASSWORD)['admin']
    assert restarted.apple_token(restarted.get(2))=='PERSISTENT_LIBRARY'
    assert not restarted.get(1)['password_hash'].endswith(PASSWORD)

def test_login_attempts_are_limited(website):
    for _ in range(10):assert website.post('/auth/login',json={'username':'unknown','password':'bad'}).status_code==401
    assert website.post('/auth/login',json={'username':'unknown','password':'bad'}).status_code==429

def test_static_web_assets_and_cookie_flags(settings):
    with TestClient(create_app(replace(settings,secure_cookies=True))) as client:
        assert client.get('/').status_code==200
        assert client.get('/assets/app.js').headers['content-type'].startswith('text/javascript')
        response=client.post('/auth/login',json={'username':'admin','password':PASSWORD})
        cookie=response.headers['set-cookie'].lower()
        assert 'secure' in cookie and 'httponly' in cookie and 'samesite=strict' in cookie


def test_expired_native_session_requires_login(website):
    create(website);login(website)
    with website.app.state.accounts.db() as db:db.execute('UPDATE sessions SET expires=0')
    assert website.get('/auth/me').status_code==401
    assert website.get('/v1/catalog/search?term=test').status_code==401


def test_expired_apple_link_does_not_expire_backend_login(website):
    uid=create(website);login(website)
    website.app.state.accounts.link(uid,'invalid','us')
    assert website.get('/v1/library/songs').status_code==409
    assert website.get('/auth/me').status_code==200
