// Browser-flow contract test with fake DOM/MusicKit; no Apple account is contacted.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync('backend/switchmusic/web/app.js','utf8');
async function scenario(qr,denied=false,cachedToken=undefined,cachedAuthorized=false){
  const nodes=new Map(),calls=[],pending=[];let gesture=false,authorized=false,cleared=false,logoutCalls=0;
  class Element{
    constructor(){this.hidden=false;this.disabled=false;this.textContent='';this.dataset={};this.classList={toggle(){}};}
    addEventListener(){}replaceChildren(){}pause(){}load(){}removeAttribute(){}append(){}setAttribute(){}reset(){}
    click(){if(!this.disabled&&this.onclick)pending.push(this.onclick());}
  }
  const node=id=>{if(!nodes.has(id))nodes.set(id,new Element());return nodes.get(id);};
  const user={username:'alice',admin:false,apple_linked:false,storefront:'us'};
  const kit={isAuthorized:cachedAuthorized,musicUserToken:cachedToken,async unauthorize(){assert.equal(typeof this.musicUserToken,'string');assert(this.musicUserToken&&this.musicUserToken!=='undefined'&&this.musicUserToken!=='null');logoutCalls++;this.isAuthorized=false;this.musicUserToken=undefined;},authorize(){if(qr)assert(gesture,'QR authorization must begin directly in the click gesture');authorized=true;return Promise.resolve('SYNTHETIC_APPLE_TOKEN');}};
  const context={URLSearchParams,queueMicrotask,console,
    location:{hash:qr?'#apple-qr='+'a'.repeat(43):'',pathname:'/',search:''},
    history:{replaceState(){cleared=true;}},
    document:{getElementById:node,querySelectorAll(){return [];},createElement(){return new Element();},addEventListener(){}},
    MusicKit:{async configure(){},getInstance(){return kit;}},
    fetch:async(path,options)=>{
      calls.push(path);let payload,status=200;
      if(path==='/auth/me')payload={user,csrf:'csrf'};
      else if(path==='/auth/apple/qr/details'){status=denied?403:200;payload=denied?{detail:'Use the same backend account'}:{username:'alice',confirmation:'123-456',expires_in:300};}
      else if(path==='/auth/apple/start'){assert.equal(Boolean(JSON.parse(options.body||'{}').qr_code),qr);payload={state:'nonce',developer_token:'SYNTHETIC_JWT'};}
      else if(path==='/auth/apple/callback'){assert.equal(JSON.parse(options.body).music_user_token,'SYNTHETIC_APPLE_TOKEN');assert.equal(Boolean(JSON.parse(options.body).qr_code),qr);payload={user:{...user,apple_linked:true}};}
      else throw Error('Unexpected request '+path);
      return {ok:status===200,status,json:async()=>payload};
    }
  };
  context.window={MusicKit:context.MusicKit};vm.createContext(context);vm.runInContext(source,context);
  for(let i=0;i<5;i++)await new Promise(setImmediate);
  if(qr){assert(cleared);assert.equal(node('apple-qr-code').textContent,denied?'':'123-456');}
  if(denied){assert(node('apple-qr-link').hidden);assert(!calls.includes('/auth/apple/start'));return;}
  if(qr){assert(!node('apple-qr-link').hidden);gesture=true;node('apple-qr-link').click();gesture=false;}
  else node('link-apple').click();
  await Promise.all(pending);for(let i=0;i<3;i++)await new Promise(setImmediate);
  assert.equal(logoutCalls,cachedAuthorized&&typeof cachedToken==='string'&&cachedToken.trim()&&cachedToken!=='undefined'&&cachedToken!=='null'?1:0);
  assert(authorized);assert(calls.includes('/auth/apple/callback'));
  assert(!calls.some(path=>path.startsWith('/auth/pairing/')));
  if(qr)assert.match(node('apple-qr-instructions').textContent,/Return to your Switch/);
}
(async()=>{await scenario(true);await scenario(false);await scenario(true,true);
for(const qr of [true,false]){for(const token of [undefined,'','undefined','null'])await scenario(qr,false,token,true);await scenario(qr,false,'CACHED_APPLE_TOKEN',true);await scenario(qr,false,'CACHED_APPLE_TOKEN',false);}
console.log('Apple QR gesture, same-account rejection and ordinary web linking passed');})().catch(error=>{console.error(error);process.exitCode=1;});
