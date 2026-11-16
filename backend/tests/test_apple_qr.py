from concurrent.futures import ThreadPoolExecutor
import hashlib
import time
import pytest
from fastapi import HTTPException
from test_accounts import website,create,login,link,USER_PASSWORD,basic
from test_backend import settings,HEADERS,PASSWORD

def start(client):
    headers=login(client)
    result=client.post('/auth/apple/qr/start',headers=headers)
    assert result.status_code==200,result.text
    return headers,result.json()

def test_qr_requires_existing_backend_session_and_csrf(website):
    assert website.post('/auth/apple/qr/start').status_code==401
    assert website.post('/auth/apple/qr/start',headers=HEADERS).status_code==400
    create(website);login(website)
    assert website.post('/auth/apple/qr/start').status_code==403
    assert website.post('/auth/apple/qr/start',headers={'X-CSRF-Token':'bad'}).status_code==403

def test_qr_contains_only_library_link_request_and_quiet_border(website,settings):
    create(website);_,flow=start(website)
    assert flow['url']=='http://testserver/#apple-qr='+flow['code']
    assert flow['expires_in']==300 and len(flow['confirmation'])==7
    assert not {'session','developer_token','music_user_token'} & flow.keys()
    matrix=flow['matrix'];assert 29<=len(matrix)<=185 and all(len(row)==len(matrix) for row in matrix)
    assert all(set(row)<={'0','1'} for row in matrix)
    assert all(set(row)=={'0'} for row in matrix[:4]+matrix[-4:])
    assert all(row[:4]==row[-4:]=='0000' for row in matrix)
    assert flow['code'].encode() not in (settings.data_dir/'accounts.sqlite3').read_bytes()

def test_phone_must_use_same_account_and_cannot_poll_native_request(website):
    uid=create(website);bob=create(website,'bob');native,flow=start(website)
    store=website.app.state.accounts;token,csrf=store.login(store.get(bob))
    body={'code':flow['code']};phone={'Cookie':'switchmusic_session='+token,'X-CSRF-Token':csrf}
    assert website.post('/auth/apple/qr/details',headers=phone,json=body).status_code==403
    assert website.post('/auth/apple/start',headers=phone,json={'qr_code':flow['code']}).status_code==403
    token,csrf=store.login(store.get(uid));phone={'Cookie':'switchmusic_session='+token,'X-CSRF-Token':csrf}
    assert website.post('/auth/apple/qr/status',headers=phone,json=body).status_code==403

def test_complete_flow_uses_existing_musickit_and_leaves_account_session_unchanged(website):
    uid=create(website);headers,flow=start(website)
    native=website.cookies.get('switchmusic_session');body={'code':flow['code']}
    # Create a distinct phone session without replacing the native cookie.
    store=website.app.state.accounts;user=store.get(uid);phone_token,csrf=store.login(user)
    phone={'Cookie':'switchmusic_session='+phone_token,'X-CSRF-Token':csrf,'Origin':'http://testserver'}
    assert website.post('/auth/apple/qr/details',headers=phone,json=body).json()['confirmation']==flow['confirmation']
    assert website.post('/auth/apple/qr/status',headers=phone,json=body).status_code==403
    state=website.post('/auth/apple/start',headers=phone,json={'qr_code':flow['code']}).json()['state']
    assert website.post('/auth/apple/callback',headers=phone,json={'state':state,'music_user_token':'QR_APPLE_SECRET','qr_code':flow['code']}).status_code==200
    result=website.post('/auth/apple/qr/status',headers=headers,json=body)
    assert result.status_code==200,result.text
    assert result.json()['status']=='linked' and result.json()['user']['apple_linked']
    assert 'session' not in result.json() and 'QR_APPLE_SECRET' not in result.text
    assert website.cookies.get('switchmusic_session')==native
    assert website.post('/auth/apple/qr/status',headers=headers,json=body).status_code==410
    assert website.get('/v1/library/songs').json()['items'][0]['title']=='QR_APPLE_SECRET'

def test_qr_expiry_cancel_replacement_and_throttle(website):
    create(website);headers,flow=start(website);body={'code':flow['code']}
    assert website.post('/auth/apple/qr/status',headers=headers,json=body).json()['status']=='pending'
    assert website.post('/auth/apple/qr/status',headers=headers,json=body).status_code==429
    assert website.post('/auth/apple/qr/cancel',headers=headers,json=body).status_code==204
    assert website.post('/auth/apple/qr/details',headers=headers,json=body).status_code==410
    first=website.post('/auth/apple/qr/start',headers=headers).json()
    second=website.post('/auth/apple/qr/start',headers=headers).json()
    assert website.post('/auth/apple/qr/details',headers=headers,json={'code':first['code']}).status_code==410
    with website.app.state.accounts.db() as db:db.execute('UPDATE apple_qr SET expires=0')
    assert website.post('/auth/apple/qr/details',headers=headers,json={'code':second['code']}).status_code==410

def test_logout_password_reset_and_unlink_invalidate_requests(website):
    uid=create(website);headers,flow=start(website)
    assert website.post('/auth/logout',headers=headers).status_code==204
    assert website.get('/auth/me').status_code==401
    headers=login(website);assert website.post('/auth/apple/qr/details',headers=headers,json={'code':flow['code']}).status_code==410
    flow=website.post('/auth/apple/qr/start',headers=headers).json()
    assert website.delete('/auth/apple',headers=headers).status_code==204
    assert website.post('/auth/apple/qr/details',headers=headers,json={'code':flow['code']}).status_code==410
    website.post('/auth/apple/qr/start',headers=headers)
    website.patch('/admin/accounts/'+str(uid),headers=HEADERS,json={'password':'Replacement-password-123'})
    with website.app.state.accounts.db() as db:assert db.execute('SELECT count(*) FROM apple_qr').fetchone()[0]==0

def test_linked_status_consumed_once_under_concurrent_requests(website):
    uid=create(website);headers,flow=start(website);store=website.app.state.accounts
    store.link(uid,'APPLE_SECRET','us',flow['code']);token=website.cookies.get('switchmusic_session')
    def claim():
        try:return store.qr(uid,flow['code'],token,'status')['status']
        except HTTPException as error:return error.status_code
    with ThreadPoolExecutor(max_workers=2) as executor:results=list(executor.map(lambda _:claim(),range(2)))
    assert sorted(map(str,results))==['410','linked']

def test_cancel_and_expiry_invalidate_musickit_nonce_and_commit(website):
    uid=create(website);headers,flow=start(website);body={'code':flow['code']}
    state=website.post('/auth/apple/start',headers=headers,json={'qr_code':flow['code']}).json()['state']
    # A bound flow cannot omit the QR code to become an ordinary web authorization.
    assert website.post('/auth/apple/callback',headers=headers,json={'state':state,'music_user_token':'SECRET'}).status_code==400
    assert website.post('/auth/apple/qr/cancel',headers=headers,json=body).status_code==204
    assert website.post('/auth/apple/callback',headers=headers,json={'state':state,'music_user_token':'SECRET','qr_code':flow['code']}).status_code==410
    assert not website.get('/auth/me').json()['user']['apple_linked']
    # Cancellation while Apple's validation is in flight is checked again before storing the token.
    store=website.app.state.accounts;flow=website.post('/auth/apple/qr/start',headers=headers).json();token=website.cookies.get('switchmusic_session')
    state=store.start_link(uid,token,flow['code']);store.consume_link(uid,token,state,flow['code'])
    store.qr(uid,flow['code'],token,'cancel')
    with pytest.raises(HTTPException) as error:store.link(uid,'SECRET','us',flow['code'])
    assert error.value.status_code==410 and store.apple_token(store.get(uid)) is None
    flow=website.post('/auth/apple/qr/start',headers=headers).json()
    state=store.start_link(uid,token,flow['code']);store.consume_link(uid,token,state,flow['code'])
    with store.db() as db:db.execute('UPDATE apple_qr SET expires=0')
    with pytest.raises(HTTPException) as error:store.link(uid,'SECRET','us',flow['code'])
    assert error.value.status_code==410 and store.apple_token(store.get(uid)) is None

def test_existing_database_adds_qr_nonce_binding(settings):
    import sqlite3
    from switchmusic.accounts import Accounts
    settings.data_dir.mkdir()
    with sqlite3.connect(settings.data_dir/'accounts.sqlite3') as db:
        db.execute('CREATE TABLE apple_flows(hash TEXT PRIMARY KEY,session_hash TEXT REFERENCES sessions(hash) ON DELETE CASCADE,user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,expires INTEGER)')
    store=Accounts(settings);user=store.authenticate('admin',PASSWORD);token,_=store.login(user)
    code,_=store.start_qr(user['id'],token);state=store.start_link(user['id'],token,code)
    store.qr(user['id'],code,token,'cancel')
    with store.db() as db:
        assert 'qr_hash' in [row['name'] for row in db.execute('PRAGMA table_info(apple_flows)')]
        assert db.execute('SELECT count(*) FROM apple_flows').fetchone()[0]==0
