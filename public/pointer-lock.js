/* Actual browser pointer lock controls a separate virtual Windows cursor.
 * Warps never request an OS warp and never produce DirectInput deltas. */
export class MoviesPointerLockHost {
  constructor({snapshot,surface,env=globalThis,onStatus=()=>{},width=1024,height=768}) {
    if(!snapshot?.setLocked||!surface||!Number.isInteger(width)||!Number.isInteger(height)||width<1||height<1||width>8192||height>8192)throw new Error('Invalid virtual pointer host');
    Object.assign(this,{snapshot,surface,env,onStatus,width,height});this.windows=new Map();this.removers=[];this.closed=false;this.active=null;this.fractionX=0;this.fractionY=0;
    this.listen(env.document,'pointerlockchange',()=>this.changed());
    this.listen(env.document,'pointerlockerror',event=>this.captureFailed(event.error??(event.message?{name:'PointerLockError',message:event.message}:null)));
    this.listen(env,'blur',()=>this.release('Mouse released because the page lost focus.'));
    this.listen(env.document,'visibilitychange',()=>{if(env.document.hidden)this.release('Mouse released because the page is hidden.');});
  }
  listen(target,type,fn,options){target.addEventListener(type,fn,options);this.removers.push(()=>target.removeEventListener(type,fn,options));}
  report(locked,message,lost=false){if(!this.closed)this.onStatus({locked,message,lost});}
  captureFailed(error){
    if(this.closed)return;
    if(error)this.captureError={name:String(error.name||'Error'),message:String(error.message||error)};
    const detail=this.captureError?`${this.captureError.name}: ${this.captureError.message}`:'browser emitted pointerlockerror without exception details';
    const d=this.captureDiagnostic||{};
    this.onStatus({locked:false,lost:false,message:`Mouse capture denied (${detail}). Visibility=${d.visibility}, focused=${d.focused}, userActivation=${d.userActivation}, requestReturnsPromise=${d.promise}.`,diagnostic:{...d,error:this.captureError??null}});
  }
  attach(canvas,hwnd){
    if(this.closed||this.windows.has(canvas))return;this.windows.set(canvas,hwnd);
    this.listen(canvas,'click',()=>this.request(canvas));
    this.listen(canvas,'pointermove',event=>{
      if(this.closed||this.active!==canvas||this.env.document.pointerLockElement!==canvas||event.pointerType!=='mouse')return;
      const rect=canvas.getBoundingClientRect();if(!rect.width||!rect.height)return;
      if(!Number.isFinite(event.movementX)||!Number.isFinite(event.movementY))return;
      this.fractionX+=event.movementX*canvas.width/rect.width;this.fractionY+=event.movementY*canvas.height/rect.height;
      const dx=Math.trunc(this.fractionX),dy=Math.trunc(this.fractionY);this.fractionX-=dx;this.fractionY-=dy;
      this.snapshot.move(dx,dy,this.width,this.height);
    },true); // before WindowSurface's message producer, not before real DI data
  }
  request(canvas){
    if(this.closed)return false;
    canvas??=this.active??[...this.windows.keys()].find(c=>!c.hidden);
    if(!canvas||!this.windows.has(canvas)||canvas.hidden||typeof canvas.requestPointerLock!=='function'){this.report(false,'Mouse capture is unavailable until the game window is visible.',false);return false;}
    if(this.env.document.pointerLockElement===canvas)return true;
    if(!this.snapshot.read()){this.report(false,'Move the mouse over the page before capturing it.',false);return false;}
    this.captureError=null;this.captureDiagnostic={visibility:this.env.document.visibilityState??'unknown',focused:this.env.document.hasFocus?.()??null,userActivation:this.env.navigator?.userActivation?.isActive??null,secureContext:this.env.isSecureContext??null,promise:null};
    try {canvas.focus();const result=canvas.requestPointerLock();this.captureDiagnostic.promise=!!result?.then;result?.catch?.(error=>this.captureFailed(error));return true;}
    catch(error){this.captureFailed(error);return false;}
  }
  changed(){
    if(this.closed)return;
    const canvas=this.env.document.pointerLockElement;
    if(canvas&&this.windows.has(canvas)){
      const point=this.snapshot.read();if(!point){this.release('Mouse capture needs a genuine pointer position.');return;}
      this.snapshot.setLocked({x:Math.min(this.width-1,Math.max(0,point.x)),y:Math.min(this.height-1,Math.max(0,point.y))});
      this.active=canvas;this.fractionX=this.fractionY=0;this.report(true,'Mouse captured. Press Escape to release it.');
    }else if(this.active||this.snapshot.isLocked())this.release('Mouse released. Click the game or Capture mouse to capture again.');
  }
  release(message){
    const wasLocked=!!this.active||this.snapshot.isLocked();this.active=null;this.fractionX=this.fractionY=0;
    if(wasLocked)this.snapshot.setLocked(null);
    if(this.windows.has(this.env.document.pointerLockElement))this.env.document.exitPointerLock?.();
    if(wasLocked)this.report(false,message,true);
  }
  close(){if(this.closed)return;this.release('Mouse released.');this.closed=true;for(const remove of this.removers)remove();this.removers=[];this.windows.clear();}
}
