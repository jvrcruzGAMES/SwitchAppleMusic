'use strict';
const $=id=>document.getElementById(id);
let csrf='',user=null,collection='',musicKitPromise=null;
const appleQRCode=new URLSearchParams(location.hash.slice(1)).get('apple-qr')||'';
let appleQRDetails=null,appleQRCompleted=false,preparedAppleFlow=null;
let appleQREpoch=0,preparingApple=false;
if(appleQRCode)history.replaceState(null,'',location.pathname+location.search);
async function loadAppleQR(){
  if(!appleQRCode||appleQRCompleted||!user)return;
  const owner=user,epoch=++appleQREpoch,active=()=>user===owner&&epoch===appleQREpoch;
  appleQRDetails=null;preparedAppleFlow=null;
  tab('account');$('apple-qr-panel').hidden=false;$('apple-qr-link').hidden=true;$('link-apple').disabled=true;
  try{
    const details=await api('/auth/apple/qr/details','POST',{code:appleQRCode});if(!active())return;
    $('apple-qr-instructions').textContent='Link the Apple Music library for '+details.username+'. Authorization happens in Apple’s window.';
    $('apple-qr-code').textContent=details.confirmation;
    await loadMusicKit();if(!active())return;
    const flow=await api('/auth/apple/start','POST',{qr_code:appleQRCode});if(!active())return;flow.qr_code=appleQRCode;
    await configureMusicKit(flow);if(!active())return;
    appleQRDetails=details;preparedAppleFlow=flow;$('apple-qr-link').hidden=false;
  }catch(error){if(active()){$('apple-qr-instructions').textContent=error.message;$('apple-qr-code').textContent='';}}
  finally{if(active())$('link-apple').disabled=false;}
}
$('apple-qr-link').onclick=()=>$('link-apple').click();
function message(text=''){ $('message').textContent=text; }
async function api(path,method='GET',body){
  const headers={};if(body!==undefined)headers['Content-Type']='application/json';
  if(method!=='GET')headers['X-CSRF-Token']=csrf;
  const response=await fetch(path,{method,headers,credentials:'same-origin',body:body===undefined?undefined:JSON.stringify(body)});
  if(!response.ok){let error={};try{error=await response.json();}catch{}if(response.status===401)showLogin();throw Error(typeof error.detail==='string'?error.detail:`Request failed (${response.status})`);}
  return response.status===204?null:response.json();
}
function clearMusic(){collection='';$('results').replaceChildren();$('accounts').replaceChildren();$('next-page').hidden=true;$('collection-title').textContent='';$('now-playing').textContent='Select a song to play';}
function showLogin(){++appleQREpoch;preparingApple=false;appleQRDetails=null;preparedAppleFlow=null;$('apple-qr-panel').hidden=true;clearMusic();user=null;csrf='';$('dashboard').hidden=true;$('login-screen').hidden=false;$('logout').hidden=true;$('identity').textContent='';$('audio').pause();$('audio').removeAttribute('src');$('audio').load();}
function refreshUser(value){user=value;$('identity').textContent=user.username;$('admin-tab').hidden=!user.admin;$('apple-status').textContent=user.apple_linked?`Library connected · ${user.storefront.toUpperCase()}`:'Your Apple Music library is not linked.';$('link-apple').textContent=user.apple_linked?'Relink Apple Music':'Link Apple Music';$('unlink-apple').hidden=!user.apple_linked;}
function signedIn(result){clearMusic();csrf=result.csrf;refreshUser(result.user);$('login-screen').hidden=true;$('dashboard').hidden=false;$('logout').hidden=false;tab('music');loadAppleQR().catch(error=>message(error.message));}
function tab(name){for(const panel of ['music','account','admin'])$(panel+'-panel').hidden=panel!==name;for(const button of document.querySelectorAll('[data-tab]'))button.classList.toggle('active',button.dataset.tab===name);if(name==='account'&&!appleQRCode&&user&&!preparedAppleFlow)prepareWebApple().catch(error=>message(error.message));if(name==='admin')loadAccounts().catch(error=>message(error.message));}
function bindForm(id,handler){$(id).addEventListener('submit',async event=>{event.preventDefault();const button=event.currentTarget.querySelector('button');button.disabled=true;message();try{await handler(Object.fromEntries(new FormData(event.currentTarget)),event.currentTarget);}catch(error){message(error.message);}finally{button.disabled=false;}});}
bindForm('login',async body=>{signedIn(await api('/auth/login','POST',body));$('login').reset();message();});
bindForm('password',async body=>{await api('/auth/password','POST',body);$('password').reset();showLogin();message('Password updated. Sign in again on your devices.');});
bindForm('create-account',async(body,form)=>{await api('/admin/accounts','POST',body);form.reset();await loadAccounts();message('Account created.');});
bindForm('search',async body=>{collection='/v1/catalog/search?term='+encodeURIComponent(body.term);$('collection-title').textContent='Catalog results';await loadMusic(collection);});
for(const button of document.querySelectorAll('[data-tab]'))button.onclick=()=>{message();tab(button.dataset.tab);};
for(const button of document.querySelectorAll('[data-library]'))button.onclick=async()=>{message();try{collection='/v1/library/'+button.dataset.library;$('collection-title').textContent='Your '+button.dataset.library;await loadMusic(collection);}catch(error){message(error.message);}};
$('logout').onclick=async()=>{try{await api('/auth/logout','POST');showLogin();message();}catch(error){message(error.message);}};
function action(text,fn,style='quiet'){const button=document.createElement('button');button.textContent=text;button.className=style;button.onclick=async()=>{button.disabled=true;message();try{await fn();}catch(error){message(error.message);}finally{button.disabled=false;}};return button;}
async function loadMusic(path){
  const owner=user;const page=await api(path);if(user!==owner)return;$('results').replaceChildren();
  for(const item of page.items){const row=document.createElement('div');row.className='row';const detail=document.createElement('div');detail.className='grow';const title=document.createElement('strong');title.textContent=item.title;const artist=document.createElement('small');artist.textContent=[item.artist,item.type].filter(Boolean).join(' · ');detail.append(title,artist);row.append(detail);
    if(['songs','library-songs'].includes(item.type))row.append(action('Play',async()=>{const song=await api('/v1/resolve/'+encodeURIComponent(item.type)+'/'+encodeURIComponent(item.id));$('now-playing').textContent=song.title+' · '+song.artist;message('Preparing audio…');$('audio').src=song.stream_path+'?codec=aac';await $('audio').play();message();}));
    else if(['albums','playlists','library-albums','library-playlists'].includes(item.type))row.append(action('Open',async()=>{const library=item.type.startsWith('library-');const kind=item.type.replace('library-','');collection='/v1/'+(library?'library':'catalog')+'/'+kind+'/'+encodeURIComponent(item.id)+'/tracks';$('collection-title').textContent=item.title;await loadMusic(collection);}));
    $('results').append(row);
  }
  if(!page.items.length){const p=document.createElement('p');p.textContent='No results found.';$('results').append(p);}
  $('next-page').hidden=page.next_offset===null||page.next_offset===undefined;
  $('next-page').onclick=()=>loadMusic(collection+(collection.includes('?')?'&':'?')+'offset='+page.next_offset).catch(error=>message(error.message));
}
async function loadAccounts(){
  const owner=user;const {accounts}=await api('/admin/accounts');if(user!==owner)return;$('accounts').replaceChildren();
  for(const account of accounts){const article=document.createElement('article');const h=document.createElement('h2');h.textContent=account.username;const status=document.createElement('p');status.textContent=[account.admin?'Administrator':account.disabled?'Disabled':'Active',account.apple_linked?'Apple library linked':'Catalog only'].join(' · ');article.append(h,status);
    if(!account.admin){const controls=document.createElement('div');controls.className='inline account-actions';controls.append(action(account.disabled?'Enable':'Disable',async()=>{await api('/admin/accounts/'+account.id,'PATCH',{disabled:!account.disabled});await loadAccounts();}));const password=document.createElement('input');password.type='password';password.placeholder='New password (12+ characters)';password.autocomplete='new-password';password.setAttribute('aria-label','New password for '+account.username);password.maxLength=256;controls.append(password,action('Reset password',async()=>{await api('/admin/accounts/'+account.id,'PATCH',{password:password.value});password.value='';await loadAccounts();message('Password reset; existing sessions revoked.');}));if(account.apple_linked)controls.append(action('Unlink library',async()=>{await api('/admin/accounts/'+account.id+'/apple','DELETE');await loadAccounts();}));controls.append(action('Delete',async()=>{if(confirm('Delete account '+account.username+' and its Apple link?')){await api('/admin/accounts/'+account.id,'DELETE');await loadAccounts();}},'danger'));article.append(controls);}
    $('accounts').append(article);
  }
}
function validMusicUserToken(token){return typeof token==='string'&&token.trim()===token&&token.length>0&&!['undefined','null'].includes(token.toLowerCase());}
function hasMusicKitAuthorization(kit){return kit.isAuthorized===true&&validMusicUserToken(kit.musicUserToken);}
async function clearMusicKitAuthorization(kit){if(hasMusicKitAuthorization(kit))await kit.unauthorize();}
async function configureMusicKit(flow){
  if(flow.developer_token_verified!==true||typeof flow.developer_token!=='string'||flow.developer_token.split('.').length!==3)
    throw Error('The backend did not return a verified MusicKit developer JWT. Rebuild the backend and retry.');
  if(!Number.isFinite(flow.expires_at)||flow.expires_at*1000<=Date.now()+60000)
    throw Error('The MusicKit developer JWT is expired. Check the server and browser clocks, then retry.');
  await MusicKit.configure({developerToken:flow.developer_token,app:{name:'SwitchMusic',build:'0.2.0'}});
  const kit=MusicKit.getInstance();
  if(kit.developerToken!==flow.developer_token)throw Error('MusicKit did not receive the backend developer JWT. Reload this page and retry.');
  flow.ready_until=Math.min(Date.now()+9*60000,flow.expires_at*1000-60000);return kit;
}
async function prepareWebApple(){
  if(preparingApple||!user)return;
  const owner=user,epoch=++appleQREpoch,active=()=>user===owner&&epoch===appleQREpoch;
  preparingApple=true;preparedAppleFlow=null;$('link-apple').disabled=true;message('Checking Apple Music setup…');
  try{
    const flow=await api('/auth/apple/start','POST');if(!active())return;
    await loadMusicKit();if(!active())return;
    await configureMusicKit(flow);if(!active())return;
    preparedAppleFlow=flow;message('Apple Music sign-in is ready.');
  }catch(error){if(active())message(error.message);}
  finally{if(active()){preparingApple=false;$('link-apple').disabled=false;}}
}
function loadMusicKit(){if(!musicKitPromise)musicKitPromise=new Promise((resolve,reject)=>{if(window.MusicKit){resolve();return;}document.addEventListener('musickitloaded',resolve,{once:true});const script=document.createElement('script');script.src='https://js-cdn.music.apple.com/musickit/v3/musickit.js';script.onerror=()=>{musicKitPromise=null;reject(Error('MusicKit could not load. Try again.'));};document.head.append(script);});return musicKitPromise;}
$('link-apple').onclick=async()=>{
  const button=$('link-apple'),owner=user;let phase='preparing';button.disabled=true;message();
  try{
    const flow=preparedAppleFlow;
    if(!flow||Date.now()>=flow.ready_until){
      if(appleQRDetails)await loadAppleQR();else await prepareWebApple();
      if(preparedAppleFlow)message('Apple Music sign-in is ready. Select Authorize Apple Music or Link Apple Music to continue.');return;
    }
    const kit=MusicKit.getInstance();
    if(hasMusicKitAuthorization(kit)){
      await clearMusicKitAuthorization(kit);
      message('Ready to choose an Apple Music account. Select Authorize Apple Music or Link Apple Music again.');return;
    }
    preparedAppleFlow=null;
    // No network/configuration await before authorize(): preserve the click gesture.
    phase='authorizing';const authorized=await kit.authorize();
    if(user!==owner)throw Error('Your account session changed. Start authorization again.');
    const token=validMusicUserToken(authorized)?authorized:(kit.isAuthorized===true?kit.musicUserToken:undefined);
    if(!validMusicUserToken(token)||token===flow.developer_token)
      throw Error('Apple Music authorization returned no Music User Token. Complete Apple’s sign-in window; the developer JWT cannot be used as a user token.');
    phase='linking';const linked=await api('/auth/apple/callback','POST',{state:flow.state,music_user_token:token,...(flow.qr_code?{qr_code:flow.qr_code}:{})});
    if(kit.musicUserToken!==token)kit.musicUserToken=token;
    refreshUser(linked.user);
    if(appleQRDetails&&!appleQRCompleted){
      appleQRCompleted=true;appleQRDetails=null;$('apple-qr-link').hidden=true;
      $('apple-qr-instructions').textContent='Apple Music connected. Return to your Switch.';
    }
    message('Apple Music library connected.');
  }catch(error){
    const detail=error.message||'Apple Music authorization did not complete.';
    message(phase==='authorizing'?'Apple accepted the developer JWT, but MusicKit authorization failed: '+detail:phase==='linking'?'Apple returned a user token, but backend validation failed: '+detail:detail);
    if(appleQRDetails&&!appleQRCompleted)queueMicrotask(()=>loadAppleQR().catch(()=>{}));
  }finally{button.disabled=false;}
};
$('unlink-apple').onclick=async()=>{try{await api('/auth/apple','DELETE');if(window.MusicKit)try{await clearMusicKitAuthorization(MusicKit.getInstance());}catch{}refreshUser((await api('/auth/me')).user);message('Apple Music library unlinked.');}catch(error){message(error.message);}};
$('audio').addEventListener('error',()=>message('Audio could not be loaded. Check your account session and server playback availability.'));
api('/auth/me').then(signedIn).catch(()=>showLogin());
