import test from 'node:test';
import assert from 'node:assert/strict';
import '../public/window-surface.js';

function fixture(onInput,pointerPosition) {
  const children = [];
  const document = {createElement(tag) {
    assert.ok(['canvas','button'].includes(tag));
    return {style: {}, attributes: {}, listeners: {}, setAttribute(k, v) { this.attributes[k] = v; },
      addEventListener(k, fn) { this.listeners[k] = fn; }, focus() {}, setPointerCapture() {},
      hasPointerCapture() { return false; }, getBoundingClientRect() { return {left: 10, top: 20, width: 400, height: 300}; },
      remove() { children.splice(children.indexOf(this), 1); }};
  }};
  return {children, surface: new MoviesWindowSurface({append(node) { children.push(node); }}, document, onInput,pointerPosition)};
}
test('window acknowledgment follows actual surface lifecycle and cursor mutations', () => {
  const {children, surface} = fixture();
  const create = {type: 'create', hwnd: 42, title: '<The Movies>', clientWidth: 800, clientHeight: 600, visible: false};
  assert.equal(surface.apply(create), 1);
  const canvas = children[0];
  assert.equal(canvas.width, 800); assert.equal(canvas.height, 600);
  assert.equal(canvas.attributes['aria-label'], '<The Movies>');
  assert.equal(canvas.hidden, true);
  assert.throws(() => surface.apply(create));
  assert.equal(children.length, 1);
  surface.apply({type: 'show', hwnd: 42, visible: true, clientWidth: 1024, clientHeight: 768});
  assert.equal(canvas.hidden, false);
  assert.equal(canvas.width, 1024); assert.equal(canvas.height, 768);
  surface.apply({type: 'cursor', hwnd: 42, cursor: 'none'});
  assert.equal(canvas.style.cursor, 'none');
  assert.throws(() => surface.apply({type: 'cursor', hwnd: 42, cursor: 'url(remote)'}));
  assert.equal(canvas.style.cursor, 'none');
  surface.apply({type: 'destroy', hwnd: 42});
  assert.equal(children.length, 0);
  assert.throws(() => surface.apply({type: 'show', hwnd: 42, visible: true}));
  surface.apply(create); surface.clear(); assert.equal(children.length, 0);
});
test('invalid windows never create a host surface', () => {
  const {children, surface} = fixture();
  for (const change of [{hwnd: 0}, {clientWidth: 8193}, {clientHeight: -1}, {type: 'unknown'}]) {
    assert.throws(() => surface.apply({type: 'create', hwnd: 1, clientWidth: 640, clientHeight: 480, ...change}));
    assert.equal(children.length, 0);
  }
});
test('mouse messages use scaled client coordinates and virtual screen origin', () => {
  const events = [];
  const {children, surface} = fixture(event => events.push(event));
  surface.apply({type: 'create', hwnd: 42, clientWidth: 800, clientHeight: 600, clientX: 16, clientY: 39});
  const pointer = {type: 'pointerdown', pointerType: 'mouse', button: 0, buttons: 1, shiftKey: true,
    clientX: 110, clientY: 70, pointerId: 1, preventDefault() {}};
  children[0].listeners.pointerdown(pointer);
  assert.equal(events[0].message, 0x201); assert.equal(events[0].wParam, 5);
  assert.equal(events[0].lParam, (100 << 16) | 200);
  assert.equal(events[0].x, 216); assert.equal(events[0].y, 139);
  children[0].listeners.pointerdown({...pointer, pointerType: 'touch'});
  assert.equal(events.length, 1);
  children[0].listeners.pointerup({...pointer, type: 'pointerup', button: 2, buttons: 0, shiftKey: false});
  assert.equal(events[1].message, 0x205); assert.equal(events[1].wParam, 0);
});

test('captured virtual POINT supplies window mouse client/screen coordinates',()=>{
 const events=[];let point={x:512,y:384};const {surface,children}=fixture(event=>events.push(event),()=>point);
 surface.apply({type:'create',hwnd:42,title:'test',clientWidth:800,clientHeight:600,clientX:16,clientY:39,visible:true});
 const event={type:'pointermove',pointerType:'mouse',clientX:999,clientY:999,buttons:0,preventDefault(){}};
 children[0].listeners.pointermove(event);assert.equal(events[0].x,512);assert.equal(events[0].y,384);assert.equal(events[0].lParam>>>0,((345<<16)|496)>>>0);
 point=null;children[0].listeners.pointermove({...event,clientX:110,clientY:70});assert.equal(events[1].x,216);assert.equal(events[1].y,139);
});


test('minimized windows request native restore before showing the canvas', () => {
  const events=[]; const {surface,children}=fixture(event=>events.push(event));
  const state={hwnd:42,clientWidth:800,clientHeight:600,visible:true,minimized:true};
  surface.apply({type:'create',...state});
  const [canvas,restore]=children;
  assert.equal(canvas.hidden,true); assert.equal(restore.hidden,false);
  restore.listeners.click();
  assert.equal(events.length,1); assert.equal(events[0].message,0x112);
  assert.equal(events[0].wParam,0xf120); assert.equal(events[0].hwnd,42);
  assert.equal(canvas.hidden,true,'No fabricated restore before guest/host acknowledgement');
  surface.apply({type:'show',...state,minimized:false});
  assert.equal(canvas.hidden,false); assert.equal(restore.hidden,true);
  restore.listeners.click(); assert.equal(events.length,1);
  surface.clear(); assert.equal(children.length,0);
});
