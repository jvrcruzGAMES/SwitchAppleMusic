from dataclasses import replace
import time
import httpx
import jwt
import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from fastapi.testclient import TestClient
from switchmusic.app import create_app
from switchmusic.apple import Apple
from test_backend import settings,PASSWORD

def login(client):
    result=client.post('/auth/login',json={'username':'admin','password':PASSWORD})
    assert result.status_code==200,result.text
    return {'X-CSRF-Token':result.json()['csrf']}

def test_signed_web_jwt_verified_with_apple_before_return_and_user_token_handoff(settings):
    key=serialization.load_pem_private_key(settings.private_key.read_bytes(),None)
    requests=[]
    def upstream(request):
        requests.append(request)
        token=request.headers['Authorization'].removeprefix('Bearer ')
        claims=jwt.decode(token,key.public_key(),algorithms=['ES256'],issuer=settings.team_id,options={'require':['iss','iat','exp']})
        header=jwt.get_unverified_header(token);assert header['alg']=='ES256' and header['kid']==settings.key_id
        assert 3590<=claims['exp']-time.time()<=3600
        if request.url.path=='/v1/catalog/us/search':
            assert claims['origin']==['http://testserver'] and request.headers['Origin']=='http://testserver'
            assert 'Music-User-Token' not in request.headers
            return httpx.Response(200,json={'data':[{'id':'us'}]})
        assert 'origin' not in claims  # backend API JWT is separate from the web-origin token
        assert request.headers['Music-User-Token']=='ACTUAL_MUSIC_USER_TOKEN'
        if request.url.path=='/v1/me/storefront':return httpx.Response(200,json={'data':[{'id':'us'}]})
        return httpx.Response(200,json={'data':[]})
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        headers=login(client)
        start=client.post('/auth/apple/start',headers=headers);assert start.status_code==200,start.text
        flow=start.json();assert flow['developer_token_verified'] is True
        assert flow['developer_token']==requests[0].headers['Authorization'].removeprefix('Bearer ')
        claims=jwt.decode(flow['developer_token'],key.public_key(),algorithms=['ES256'])
        assert flow['expires_at']==claims['exp']
        assert client.post('/auth/apple/callback',headers=headers,json={'state':flow['state'],'music_user_token':'ACTUAL_MUSIC_USER_TOKEN'}).status_code==200
        assert client.get('/v1/library/songs').status_code==200
        assert client.app.state.accounts.apple_token(client.app.state.accounts.get(1))=='ACTUAL_MUSIC_USER_TOKEN'
        assert len(requests)==3

@pytest.mark.parametrize('status',[401,403])
def test_apple_rejection_blocks_jwt_delivery_and_nonce_creation(settings,status):
    with TestClient(create_app(settings,httpx.MockTransport(lambda request:httpx.Response(status,json={})))) as client:
        response=client.post('/auth/apple/start',headers=login(client))
        assert response.status_code==503 and f'HTTP {status}' in response.json()['detail']
        assert 'developer_token' not in response.json()
        with client.app.state.accounts.db() as db:assert db.execute('SELECT count(*) FROM apple_flows').fetchone()[0]==0

@pytest.mark.parametrize('change',[{'team_id':'bad'},{'key_id':''}])
def test_invalid_identifiers_report_configuration_error(settings,change):
    with TestClient(create_app(replace(settings,**change))) as client:
        response=client.post('/auth/apple/start',headers=login(client))
        assert response.status_code==503 and '10-character' in response.json()['detail']

def test_wrong_key_curve_and_missing_file_are_rejected(settings):
    settings.private_key.write_bytes(ec.generate_private_key(ec.SECP384R1()).private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    with TestClient(create_app(settings)) as client:
        headers=login(client)
        assert client.post('/auth/apple/start',headers=headers).status_code==503
        settings.private_key.unlink()
        response=client.post('/auth/apple/start',headers=headers)
        assert response.status_code==503 and 'permissions' in response.json()['detail']
        assert str(settings.private_key) not in response.text

@pytest.mark.parametrize('token',['undefined','null','Undefined',' ',' invalid '])
def test_placeholder_user_token_never_reaches_apple_or_storage(settings,token):
    paths=[]
    def upstream(request):paths.append(request.url.path);return httpx.Response(200,json={'data':[{'id':'us'}]})
    with TestClient(create_app(settings,httpx.MockTransport(upstream))) as client:
        headers=login(client);state=client.post('/auth/apple/start',headers=headers).json()['state']
        response=client.post('/auth/apple/callback',headers=headers,json={'state':state,'music_user_token':token})
        assert response.status_code==422
        assert paths==['/v1/catalog/us/search'] and client.app.state.accounts.apple_token(client.app.state.accounts.get(1)) is None

async def test_preflight_cached_without_extending_signed_token_expiry(settings):
    requests=[]
    def upstream(request):requests.append(request);return httpx.Response(200,json={'data':[{'id':'us'}]})
    async with httpx.AsyncClient(transport=httpx.MockTransport(upstream)) as http:
        apple=Apple(settings,http)
        one=await apple.authorization_token();two=await apple.authorization_token()
        assert one==two and len(requests)==1
        assert one['expires_at']==jwt.decode(one['developer_token'],options={'verify_signature':False})['exp']


def test_musickit_popup_receives_origin_only_referrer_policy(settings):
    with TestClient(create_app(settings,httpx.MockTransport(lambda request:httpx.Response(200,json={})))) as client:
        for path in ('/', '/assets/app.js'):
            response=client.get(path)
            assert response.status_code==200
            assert response.headers['Referrer-Policy']=='strict-origin'
            assert response.headers['Cache-Control']=='private, no-store'
