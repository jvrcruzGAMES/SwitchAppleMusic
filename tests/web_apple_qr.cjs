// Browser-flow contract test with fake DOM/MusicKit; no Apple account is contacted.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync('backend/switchmusic/web/app.js','utf8');
async function scenario(qr,denied=false,cachedToken=undefined,cachedAuthorized=false,options={}){
  const nodes=new Map(),calls=[],pending=[];let gesture=false,authorized=false,cleared=false,logoutCalls=0,configured=false;
  const developerJWT=Buffer.from(JSON.stringify({alg:'ES256',kid:'KEY1234567'})).toString('base64url')+'.'+Buffer.from(JSON.stringify({iss:'TEAM123456',exp:Math.floor(Date.now()/1000)+3600})).toString('base64url')+'.signature';
  class Element{
    constructor(){this.hidden=false;this.disabled=false;this.textContent='';this.dataset={};this.classList={toggle(){}};}
    addEventListener(){}replaceChildren(){}pause(){}load(){}removeAttribute(){}append(){}setAttribute(){}reset(){}
    click(){if(!this.disabled&&this.onclick)pending.push(this.onclick());}
  }
  const node=id=>{if(!nodes.has(id))nodes.set(id,new Element());return nodes.get(id);};
  const accountTab=node('account-tab');accountTab.dataset.tab='account';
  const user={username:'alice',admin:false,apple_linked:false,storefront:'us'};
  const kit={isAuthorized:cachedAuthorized,musicUserToken:cachedToken,
    async unauthorize(){assert.equal(typeof this.musicUserToken,'string');assert(this.musicUserToken&&this.musicUserToken!=='undefined'&&this.musicUserToken!=='null');logoutCalls++;this.isAuthorized=false;this.musicUserToken=undefined;},
    authorize(){
      assert(gesture,'Apple authorization must begin directly in the click gesture');assert(configured);assert.equal(this.developerToken,developerJWT);authorized=true;
      if(options.rejectedAuthorization)return Promise.reject(Error('Unauthorized'));
      const token=options.returnDeveloperJWT?developerJWT:Object.hasOwn(options,'returnedToken')?options.returnedToken:'SYNTHETIC_APPLE_TOKEN';
      this.musicUserToken=Object.hasOwn(options,'sdkToken')?options.sdkToken:token;this.isAuthorized=Boolean(this.musicUserToken);
      return Promise.resolve(token);
    }
  };
  const context={URLSearchParams,queueMicrotask,console,
    location:{hash:qr?'#apple-qr='+'a'.repeat(43):'',pathname:'/',search:''},
    history:{replaceState(){cleared=true;}},
    document:{getElementById:node,querySelectorAll(selector){return selector==='[data-tab]'?[accountTab]:[];},createElement(){return new Element();},addEventListener(){}},
    MusicKit:{async configure(config){assert.equal(config.developerToken,developerJWT);kit.developerToken=options.mismatchedJWT?'wrong':config.developerToken;configured=true;},getInstance(){return kit;}},
    fetch:async(path,request)=>{
      calls.push(path);let payload,status=200;
      if(path==='/auth/me')payload={user,csrf:'csrf'};
      else if(path==='/auth/apple/qr/details'){status=denied?403:200;payload=denied?{detail:'Use the same backend account'}:{username:'alice',confirmation:'123-456',expires_in:300};}
      else if(path==='/auth/apple/start'){
        assert.equal(Boolean(JSON.parse(request.body||'{}').qr_code),qr);
        if(options.rejectedJWT){status=503;payload={detail:'Apple rejected the MusicKit developer JWT'};}
        else payload={state:'nonce',developer_token:developerJWT,developer_token_verified:!options.unverifiedJWT,expires_at:Math.floor(Date.now()/1000)+3600};
      }
      else if(path==='/auth/apple/callback'){
        const body=JSON.parse(request.body);assert.equal(body.music_user_token,'SYNTHETIC_APPLE_TOKEN');assert.notEqual(body.music_user_token,developerJWT);assert.equal(Boolean(body.qr_code),qr);
        payload={user:{...user,apple_linked:true}};
      }else throw Error('Unexpected request '+path);
      return {ok:status===200,status,json:async()=>payload};
    }
  };
  context.window={MusicKit:context.MusicKit};vm.createContext(context);vm.runInContext(source,context);
  const settle=async()=>{await Promise.all(pending);pending.length=0;for(let i=0;i<5;i++)await new Promise(setImmediate);};
  await settle();if(!qr){accountTab.click();await settle();}
  if(qr)assert(cleared);
  if(denied){assert(node('apple-qr-link').hidden);assert(!calls.includes('/auth/apple/start'));return;}
  if(options.rejectedJWT||options.unverifiedJWT||options.mismatchedJWT){assert(!authorized);assert(!calls.includes('/auth/apple/callback'));return;}
  assert(!node(qr?'apple-qr-link':'link-apple').disabled);
  const click=()=>{gesture=true;node(qr?'apple-qr-link':'link-apple').click();gesture=false;};
  click();await settle();
  const validCached=cachedAuthorized&&typeof cachedToken==='string'&&cachedToken.trim()&&cachedToken!=='undefined'&&cachedToken!=='null';
  if(validCached){assert(!authorized);click();await settle();}
  assert.equal(logoutCalls,validCached?1:0);assert(authorized);
  if(options.rejectedAuthorization){assert(!calls.includes('/auth/apple/callback'));assert.match(node('message').textContent,/Apple accepted the developer JWT, but MusicKit authorization failed/);return;}
  if(options.invalidUserToken){assert(!calls.includes('/auth/apple/callback'));assert.match(node('message').textContent,/no Music User Token/);return;}
  assert.equal(kit.musicUserToken,'SYNTHETIC_APPLE_TOKEN');
  assert(calls.includes('/auth/apple/callback'));assert(!calls.some(path=>path.startsWith('/auth/pairing/')));
  if(qr)assert.match(node('apple-qr-instructions').textContent,/Return to your Switch/);
}
(async()=>{
  await scenario(true);await scenario(false);await scenario(true,true);
  for(const qr of [true,false]){
    for(const token of [undefined,'','undefined','null'])await scenario(qr,false,token,true);
    await scenario(qr,false,'CACHED_APPLE_TOKEN',true);await scenario(qr,false,'CACHED_APPLE_TOKEN',false);
    for(const returnedToken of [undefined,'','undefined','null',' '])await scenario(qr,false,undefined,false,{returnedToken,invalidUserToken:true});
    await scenario(qr,false,undefined,false,{returnedToken:undefined,sdkToken:'SYNTHETIC_APPLE_TOKEN'});
    await scenario(qr,false,undefined,false,{sdkToken:undefined});
    await scenario(qr,false,undefined,false,{rejectedAuthorization:true});
    await scenario(qr,false,undefined,false,{returnDeveloperJWT:true,invalidUserToken:true});
    for(const flag of ['rejectedJWT','unverifiedJWT','mismatchedJWT'])await scenario(qr,false,undefined,false,{[flag]:true});
  }
  console.log('Verified JWT configuration, direct-click authorization, token handoff, QR and ordinary web linking passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
