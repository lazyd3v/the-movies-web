'use strict';
(function(root) {
  // Single producer (UI), single consumer (Wasm worker). Publish a slot only
  // after writing every field; release it only after the consumer copies it.
  const fields = ['hwnd', 'message', 'wParam', 'lParam', 'time', 'x', 'y'];
  root.MoviesPointerSnapshot = class {
    static create() { return new SharedArrayBuffer(32); }
    constructor(buffer) {
      if (!(buffer instanceof SharedArrayBuffer) || ![16,32].includes(buffer.byteLength)) throw new Error('Invalid pointer snapshot');
      this.data = new Int32Array(buffer,0,4);
      this.virtual=buffer.byteLength===32?new BigUint64Array(buffer,24,1):null;
    }
    write(point) {
      if (point && [point.x,point.y].some(n=>!Number.isInteger(n)||n < -2147483648||n > 2147483647)) throw new Error('Invalid pointer coordinates');
      Atomics.add(this.data,0,1);
      if (point) { Atomics.store(this.data,2,point.x); Atomics.store(this.data,3,point.y); }
      Atomics.store(this.data,1,point ? 1 : 0); Atomics.add(this.data,0,1);
      Atomics.notify(this.data,0);
      // A genuine absolute sample supersedes retained virtual coordinates only
      // after physical publication, and never overwrites an active capture.
      if(point&&this.virtual){let old=Atomics.load(this.virtual,0);for(let i=0;i<64&&!((old>>32n)&1n)&&((old>>32n)&2n);i++){
        const actual=Atomics.compareExchange(this.virtual,0,old,old&~(2n<<32n));if(actual===old)break;old=actual;
      }}
    }
    // One atomic word owns generation/capture flag and both virtual coordinates.
    // Physical POINT remains signed32; virtual desktop coordinates fit unsigned16.
    isLocked() { return !!(this.virtual && (Atomics.load(this.virtual,0)>>32n)&1n); }
    setLocked(point) {
      if(!this.virtual)throw new Error('Virtual cursor storage unavailable');
      if(point && [point.x,point.y].some(n=>!Number.isInteger(n)||n<0||n>65535))throw new Error('Invalid virtual cursor point');
      for(let i=0;i<64;i++){
        const old=Atomics.load(this.virtual,0),generation=((old>>32n)+4n)&0xfffffffcn;
        const next=((generation|(point?3n:((old>>32n)&2n)))<<32n)|(point?(BigInt(point.y)<<16n)|BigInt(point.x):(old&0xffffffffn));
        if(Atomics.compareExchange(this.virtual,0,old,next)===old){if(!point)this.write(null);return;}
      }
      throw new Error('Virtual cursor capture publication contention');
    }
    warp(x,y) {
      if(!this.virtual||![x,y].every(n=>Number.isInteger(n)&&n>=0&&n<=65535))return false;
      let old=Atomics.load(this.virtual,0);if(!((old>>32n)&1n))return false;
      const generation=old&0xffffffff00000000n;
      for(let i=0;i<64;i++){
        if((old&0xffffffff00000000n)!==generation)return false;
        const next=generation|(BigInt(y)<<16n)|BigInt(x),actual=Atomics.compareExchange(this.virtual,0,old,next);
        if(actual===old)return true;old=actual;
      }
      return false;
    }
    move(dx,dy,width,height) {
      if(![dx,dy,width,height].every(Number.isInteger)||width<1||height<1||width>65536||height>65536)throw new Error('Invalid virtual cursor delta/bounds');
      if(!this.virtual)return null;
      for(let i=0;i<64;i++){
        const old=Atomics.load(this.virtual,0);if(!((old>>32n)&1n))return null;
        const x=Math.min(width-1,Math.max(0,Number(old&65535n)+dx)),y=Math.min(height-1,Math.max(0,Number((old>>16n)&65535n)+dy));
        const next=(old&0xffffffff00000000n)|(BigInt(y)<<16n)|BigInt(x);
        if(Atomics.compareExchange(this.virtual,0,old,next)===old)return {x,y};
      }
      throw new Error('Virtual cursor movement contention');
    }
    // null = published unavailable; undefined = writer busy (not missing input).
    read() {
      const retained=this.virtual?Atomics.load(this.virtual,0):0n;
      const virtualPoint=()=>({x:Number(retained&65535n),y:Number((retained>>16n)&65535n)});
      if((retained>>32n)&1n)return virtualPoint();
      for (let n=0;n<8;n++) {
        const sequence=Atomics.load(this.data,0); if(sequence&1)continue;
        const valid=Atomics.load(this.data,1),x=Atomics.load(this.data,2),y=Atomics.load(this.data,3);
        if(sequence===Atomics.load(this.data,0))return valid ? {x,y} : ((retained>>32n)&2n)?virtualPoint():null;
      }
      return undefined;
    }
    // Worker-only synchronous bridge. Never block the page publishing the point.
    readBlocking(timeoutMs = 50) {
      if (!Number.isFinite(timeoutMs) || timeoutMs <= 0 || timeoutMs > 1000) throw new Error('Invalid pointer wait deadline');
      if (typeof document !== 'undefined') throw new Error('Pointer snapshot wait requires a worker');
      const deadline=performance.now()+timeoutMs;
      for (;;) {
        const point=this.read(); if(point!==undefined)return point;
        const remaining=deadline-performance.now();
        if(remaining<=0)throw new Error('Pointer snapshot publisher stalled');
        const sequence=Atomics.load(this.data,0);
        // A completed publication between read and wait must be read immediately.
        if(sequence&1)Atomics.wait(this.data,0,sequence,remaining);
      }
    }
  };
  root.MoviesWindowInput = class {
    static create(capacity = 256, layout = fields) {
      if (!Number.isInteger(capacity) || capacity < 2 || capacity > 4096 || (capacity & (capacity - 1))) throw new Error('Invalid input capacity');
      if (!Array.isArray(layout) || !layout.length || layout.length > 16) throw new Error('Invalid input fields');
      const buffer = new SharedArrayBuffer((4 + capacity * layout.length) * 4);
      new Int32Array(buffer)[3] = capacity;
      return buffer;
    }
    constructor(buffer, layout = fields) {
      if (!Array.isArray(layout) || !layout.length || layout.length > 16 || new Set(layout).size !== layout.length || layout.some(key => typeof key !== 'string')) throw new Error('Invalid input fields');
      this.fields = [...layout];
      if (!(buffer instanceof SharedArrayBuffer) || buffer.byteLength < 24 || buffer.byteLength % 4) throw new Error('Invalid shared input buffer');
      this.data = new Int32Array(buffer);
      this.capacity = this.data[3];
      if (this.capacity < 2 || this.capacity > 4096 || (this.capacity & (this.capacity - 1)) || this.data.length !== 4 + this.capacity * this.fields.length) throw new Error('Invalid shared input layout');
    }
    push(event) {
      const fields = this.fields;
      for (const key of fields) if (!Number.isInteger(event[key]) || event[key] < -2147483648 || event[key] > 4294967295) throw new Error('Invalid input field');
      const write = Atomics.load(this.data, 0) >>> 0;
      const read = Atomics.load(this.data, 1) >>> 0;
      if (((write - read) >>> 0) >= this.capacity) {
        Atomics.store(this.data, 2, 1);
        return false;
      }
      const offset = 4 + (write % this.capacity) * fields.length;
      fields.forEach((key, i) => { this.data[offset + i] = event[key]; });
      Atomics.store(this.data, 0, (write + 1) | 0);
      return true;
    }
    drain(maxCount) {
      const fields = this.fields;
      if (!Number.isInteger(maxCount) || maxCount < 0 || maxCount > 4096) throw new Error('Invalid input read count');
      if (Atomics.load(this.data, 2)) throw new Error('Game input queue overflowed');
      let read = Atomics.load(this.data, 1) >>> 0;
      const write = Atomics.load(this.data, 0) >>> 0;
      const available = (write - read) >>> 0;
      if (available > this.capacity) throw new Error('Invalid input queue counters');
      const count = Math.min(available, maxCount);
      const events = [];
      for (let n = 0; n < count; n++) {
        const offset = 4 + (read % this.capacity) * fields.length;
        const event = {};
        fields.forEach((key, i) => { event[key] = key === 'x' || key === 'y' ? this.data[offset + i] : this.data[offset + i] >>> 0; });
        events.push(event); read = (read + 1) >>> 0;
      }
      Atomics.store(this.data, 1, read | 0);
      return events;
    }
  };
})(globalThis);
