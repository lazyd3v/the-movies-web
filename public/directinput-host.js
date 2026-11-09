'use strict';
(function(root) {
  // Physical DIK positions from Wine 11.0 include/dinput.h; layout-independent.
  const keys = {Escape:1,Minus:12,Equal:13,Backspace:14,Tab:15,BracketLeft:26,BracketRight:27,
    Enter:28,ControlLeft:29,Semicolon:39,Quote:40,Backquote:41,ShiftLeft:42,Backslash:43,
    Comma:51,Period:52,Slash:53,ShiftRight:54,NumpadMultiply:55,AltLeft:56,Space:57,CapsLock:58,
    NumLock:69,ScrollLock:70,Numpad7:71,Numpad8:72,Numpad9:73,NumpadSubtract:74,Numpad4:75,
    Numpad5:76,Numpad6:77,NumpadAdd:78,Numpad1:79,Numpad2:80,Numpad3:81,Numpad0:82,
    NumpadDecimal:83,IntlBackslash:86,F11:87,F12:88,NumpadEnter:156,ControlRight:157,
    NumpadDivide:181,PrintScreen:183,AltRight:184,Pause:197,Home:199,ArrowUp:200,PageUp:201,
    ArrowLeft:203,ArrowRight:205,End:207,ArrowDown:208,PageDown:209,Insert:210,Delete:211,
    MetaLeft:219,MetaRight:220,ContextMenu:221};
  for (const [letters, first] of [['QWERTYUIOP',16],['ASDFGHJKL',30],['ZXCVBNM',44]]) {
    [...letters].forEach((letter,i) => { keys['Key'+letter] = first+i; });
  }
  for(let i=1;i<=9;i++) keys['Digit'+i]=i+1;
  keys.Digit0=11;
  for(let i=1;i<=10;i++) keys['F'+i]=58+i;
  root.MoviesDirectInputKeys = Object.freeze(keys);
  root.MoviesDirectInputFields = Object.freeze(['deviceKind','offset','data','time']);
  const asyncSlots=Object.freeze({0x1b:2,0x20:3,0x0d:4,0x21:5,0x22:6,0x25:7,0x27:8,0x26:9,0x28:10,0x01:11,0x02:12,0x04:13});
  const keyboardSlots=Object.freeze({1:2,57:3,28:4,156:4,201:5,209:6,203:7,205:8,200:9,208:10});
  root.MoviesAsyncKeyControlBytes=56;
  // control[0]=ready,[1]=focused HWND,[2..13]=the observed keyboard/mouse VKs.
  // Each key word: bit0 currently down, bit1 pressed since last atomic query.
  root.MoviesReadAsyncKeyState = (control, vk) => {
    const slot = asyncSlots[vk];
    if (slot===undefined) throw new Error(`Unsupported GetAsyncKeyState virtual key 0x${Number(vk).toString(16)}`);
    if (!(control instanceof Int32Array) || !(control.buffer instanceof SharedArrayBuffer) || control.length<=slot)
      throw new Error('Shared asynchronous key state unavailable');
    if (Atomics.load(control,0)!==1 || !Atomics.load(control,1)) return 0;
    const previous=Atomics.and(control,slot,~2);
    if (Atomics.load(control,0)!==1 || !Atomics.load(control,1)) return 0;
    return (previous&1?0x8000:0)|(previous&2?1:0);
  };
  root.MoviesDirectInputHost = class {
    constructor(buffer, control, failed, env = root) {
      this.queue = new MoviesWindowInput(buffer, root.MoviesDirectInputFields);
      this.control = new Int32Array(control);
      this.failed = failed; this.env = env; this.windows = new Map(); this.removers = [];
      this.held = new Set(); this.motionResets = new Set();
      Atomics.store(this.control, 0, 1);
      this.listen(env, 'blur', () => this.blur());
      const refreshFocus = () => {
        this.blur();
        for (const [hwnd,canvas] of this.windows) if (env.document.activeElement === canvas && !canvas.hidden) this.focus(hwnd);
      };
      this.listen(env, 'focus', refreshFocus);
      if (env.document.addEventListener) { this.listen(env.document, 'visibilitychange', refreshFocus); this.listen(env.document, 'pointerlockchange', () => { for(const reset of this.motionResets)reset(); }); }
    }
    listen(target, type, fn, options) {
      target.addEventListener(type, fn, options);
      this.removers.push(() => target.removeEventListener(type, fn, options));
    }
    focus(hwnd) {
      const canvas=this.windows.get(hwnd),doc=this.env.document;
      if (!canvas || canvas.hidden || doc.hidden || doc.activeElement!==canvas ||
          (typeof doc.hasFocus==='function' && !doc.hasFocus())) { this.blur(); return; }
      Atomics.store(this.control, 1, hwnd);
    }
    // Re-derive focus from current DOM state; focus events can be missed
    // (e.g. returning to the window while the canvas is already active).
    sync() {
      const doc=this.env.document,current=Atomics.load(this.control,1);
      let target=0;
      if(!doc.hidden && (typeof doc.hasFocus!=='function' || doc.hasFocus()))
        for(const [hwnd,canvas] of this.windows) if(doc.activeElement===canvas && !canvas.hidden) target=hwnd;
      if(target===current) return target;
      if(target) Atomics.store(this.control,1,target); else this.blur();
      return target;
    }
    blur() {
      Atomics.store(this.control, 1, 0); this.held.clear();for(const reset of this.motionResets)reset();
      for(let slot=2;slot<Math.min(14,this.control.length);slot++)Atomics.store(this.control,slot,0);
    }
    emit(deviceKind, offset, data) {
      const time = Math.floor(performance.timeOrigin + performance.now()) >>> 0;
      // DirectInput loses buffered data on overflow (DI_BUFFEROVERFLOW); it never
      // terminates the application. Motion keeps room for button/key edges.
      if (!this.queue.offer({deviceKind, offset, data: data >>> 0, time}, deviceKind === 2 && offset < 12 ? this.queue.capacity >> 2 : 0)) this.dropped = (this.dropped || 0) + 1;
    }
    attach(canvas, hwnd) {
      this.windows.set(hwnd,canvas);
      this.listen(canvas,'focus',()=>this.focus(hwnd));
      this.listen(canvas,'blur',()=>{ if (Atomics.load(this.control,1) === hwnd) this.blur(); });
      const key = e => {
        if (Atomics.load(this.control,1) !== hwnd) return;
        const offset = keys[e.code]; if (offset === undefined) return;
        const down = e.type === 'keydown';
        if (down ? !this.held.has(offset) : this.held.has(offset)) {
          if(down)this.held.add(offset);else this.held.delete(offset);
          const slot=keyboardSlots[offset];
          if(slot!==undefined && this.control.length>slot) {
            const stillDown=slot===4?(this.held.has(28)||this.held.has(156)):this.held.has(offset);
            if(down)Atomics.or(this.control,slot,3); // genuine new physical press, not repeat
            else if(!stillDown)Atomics.and(this.control,slot,~1); // retain unconsumed press edge
          }
          this.emit(1,offset,down?128:0);
        }
        e.preventDefault();
      };
      this.listen(canvas,'keydown',key); this.listen(canvas,'keyup',key);
      let last = null, fractionX = 0, fractionY = 0;
      const resetMotion=()=>{last=null;fractionX=fractionY=0;};this.motionResets.add(resetMotion);this.removers.push(()=>this.motionResets.delete(resetMotion));
      this.listen(canvas,'pointermove',e=>{
        if(e.pointerType !== 'mouse' || Atomics.load(this.control,1) !== hwnd)return;
        const rect = canvas.getBoundingClientRect(); if(!rect.width || !rect.height)return;
        const dx = Number.isFinite(e.movementX) ? e.movementX : last ? e.clientX-last.x : 0;
        const dy = Number.isFinite(e.movementY) ? e.movementY : last ? e.clientY-last.y : 0;
        last={x:e.clientX,y:e.clientY};
        fractionX+=dx*canvas.width/rect.width; fractionY+=dy*canvas.height/rect.height;
        const x=Math.trunc(fractionX),y=Math.trunc(fractionY);fractionX-=x;fractionY-=y;
        if(x)this.emit(2,0,x);if(y)this.emit(2,4,y);
      });
      const button = e => {
        if(e.pointerType !== 'mouse' || Atomics.load(this.control,1) !== hwnd)return;
        const offset = [12,14,13,15][e.button]; if(offset===undefined)return;
        const slot=[11,13,12][e.button],down=e.type==='pointerdown';
        if(slot!==undefined && this.control.length>slot) {
          if(down) { if(!(Atomics.load(this.control,slot)&1))Atomics.or(this.control,slot,3); }
          else Atomics.and(this.control,slot,~1);
        }
        this.emit(2,offset,down?128:0);
      };
      this.listen(canvas,'pointerdown',button); this.listen(canvas,'pointerup',button);
      this.listen(canvas,'pointercancel',()=>{for(let slot=11;slot<Math.min(14,this.control.length);slot++)Atomics.store(this.control,slot,0);});
      this.listen(canvas,'wheel',e=>{
        if(Atomics.load(this.control,1)!==hwnd)return;
        // DOM pixel deltas map to relative wheel units; line/page modes use
        // 40/120 units. This is an explicit virtual mouse convention.
        const delta=Math.round(-e.deltaY*(e.deltaMode===1?40:e.deltaMode===2?120:1));
        if(delta)this.emit(2,8,delta);e.preventDefault();
      },{passive:false});
    }
    detach(hwnd) { if(Atomics.load(this.control,1)===hwnd)this.blur();this.windows.delete(hwnd); }
    dispose() {
      Atomics.store(this.control,0,0);this.blur();
      for(const remove of this.removers)remove();this.removers=[];this.windows.clear();
    }
  };
})(globalThis);
