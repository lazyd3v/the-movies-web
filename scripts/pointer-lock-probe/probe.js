import {MoviesPointerLockHost} from '../../pointer-lock.js';
const status=document.querySelector('#status'),output=document.querySelector('#result'),capture=document.querySelector('#capture'),finish=document.querySelector('#finish');
const buffer=MoviesPointerSnapshot.create(),snapshot=new MoviesPointerSnapshot(buffer),control=new SharedArrayBuffer(MoviesAsyncKeyControlBytes),queueBuffer=MoviesWindowInput.create(1024,MoviesDirectInputFields),queue=new MoviesWindowInput(queueBuffer,MoviesDirectInputFields);
const events=[],checks=[],pending=new Map();let id=0,ready=false,captureVerified=false,movementVerified=false,previous=null;
const worker=new Worker('./worker.js',{type:'module'});
function show(){output.textContent=JSON.stringify({checks,point:snapshot.read(),captured:snapshot.isLocked(),movementVerified},null,2);}
function fail(error){status.textContent='FAILED';globalThis.pointerProbeResult={status:'failed',error:String(error.stack||error),checks};show();}
function request(x,y){return new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});worker.postMessage({type:'warp',id:n,x,y});});}
const surface=new MoviesWindowSurface(document.querySelector('#surface'),document,event=>{events.push(event);if(events.length>1000)events.shift();},()=>snapshot.isLocked()?snapshot.read():null);
const host=new MoviesPointerLockHost({snapshot,surface,onStatus:async state=>{status.textContent=state.message;if(state.diagnostic){globalThis.pointerProbeResult={status:'capture-denied',diagnostic:state.diagnostic,checks};output.textContent=JSON.stringify(pointerProbeResult,null,2);}if(state.lost){di.blur();capture.focus();finish.disabled=!captureVerified;if(ready){const released=await request(1,2);if(released.warped||released.error!==50||!released.read){fail(Error('Capture loss lost the valid virtual cursor'));return;}checks.push({nativeReleasedCursor:released});show();}}
 if(state.locked&&ready){try{
  queue.drain(1024);events.length=0;
  const reply=await request(512,384);if(!reply.warped||reply.error!==123||reply.point.x!==512||reply.point.y!==384)throw Error('Native captured warp mismatch');
  const generated=queue.drain(1024);if(generated.length)throw Error('Warp generated artificial DirectInput events');checks.push({nativeCapturedWarp:reply,noSyntheticDIMotion:true});
  const clamped=await request(-2147483648,2147483647);if(!clamped.warped||clamped.point.x!==0||clamped.point.y!==767)throw Error('Native desktop clamp mismatch');checks.push({clamped});
  await request(512,384);previous={x:512,y:384};captureVerified=true;status.textContent='Native warp passed. Move the mouse, then press Escape and Verify release.';show();
 }catch(error){fail(error);}}
}});
const di=new MoviesDirectInputHost(queueBuffer,control,message=>fail(Error(message)));
surface.apply({type:'create',hwnd:42,title:'Pointer diagnostic',clientWidth:1024,clientHeight:768,clientX:0,clientY:0,visible:true,minimized:false});
const canvas=surface.windows.get(42);host.attach(canvas,42);di.attach(canvas,42);
function physical(event){if(event.pointerType!=='mouse'||snapshot.isLocked())return;const r=canvas.getBoundingClientRect();snapshot.write({x:Math.floor((event.clientX-r.left)*canvas.width/r.width),y:Math.floor((event.clientY-r.top)*canvas.height/r.height)});}
for(const type of ['pointermove','pointerdown','pointerup'])document.addEventListener(type,physical,true);
canvas.addEventListener('pointermove',event=>{if(!snapshot.isLocked()||!captureVerified)return;try{
 const point=snapshot.read(),message=events.at(-1),deltas=queue.drain(1024),dx=deltas.filter(x=>x.offset===0).reduce((s,x)=>s+(x.data|0),0),dy=deltas.filter(x=>x.offset===4).reduce((s,x)=>s+(x.data|0),0);
 if(!message||message.message!==512||message.x!==point.x||message.y!==point.y)throw Error('WM_MOUSEMOVE differs from virtual cursor');
 const expected={x:Math.min(1023,Math.max(0,previous.x+dx)),y:Math.min(767,Math.max(0,previous.y+dy))};if(point.x!==expected.x||point.y!==expected.y)throw Error('Real relative DI movement differs from virtual cursor');
 if(dx||dy){movementVerified=true;checks.push({actualMovement:{dx,dy,point,windowMessage:{x:message.x,y:message.y}}});if(checks.length>30)checks.splice(2,1);}previous=point;show();
 }catch(error){fail(error);}});
capture.onclick=()=>host.request();
finish.onclick=async()=>{try{if(snapshot.isLocked())throw Error('Press Escape to release first');const reply=await request(1,2);if(reply.warped||reply.error!==50||!reply.read)throw Error('Unlocked failure or retained position mismatch');if(!movementVerified)throw Error('No actual captured mouse movement verified');checks.push({nativeUnlockedWarp:reply});host.close();di.dispose();worker.terminate();globalThis.pointerProbeResult={status:'passed',nativeWasm:true,actualPointerLock:true,checks};status.textContent='PASSED — actual capture, native warp, real movement and release';output.textContent=JSON.stringify(pointerProbeResult,null,2);}catch(error){fail(error);}};
worker.onmessage=async({data})=>{if(data.type==='ready'){ready=true;const reply=await request(512,384);if(reply.warped||reply.error!==50){fail(Error('Unlocked warp must fail'));return;}checks.push({initialUnlocked:reply});capture.disabled=false;status.textContent='Ready. Click Capture mouse.';show();}else if(data.type==='reply'){pending.get(data.id)?.resolve(data);pending.delete(data.id);}else fail(Error(data.message));};
worker.onerror=event=>fail(Error(event.message));worker.postMessage({type:'init',buffer});
