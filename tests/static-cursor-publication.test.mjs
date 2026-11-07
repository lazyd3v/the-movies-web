import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source=fs.readFileSync(new URL('../public/static-probe.js',import.meta.url),'utf8');
const publish=source.slice(source.indexOf('function publishPointer()'),source.indexOf('function observePointer('));
function fixture(point) {
  const geometry={clientX:16,clientY:39,minimized:true};
  const canvas={width:800,height:600,getBoundingClientRect:()=>geometry.minimized
    ? {left:0,top:0,width:0,height:0} : {left:10,top:20,width:400,height:300}};
  const snapshot={read:()=>point,write:value=>{point=value;}};
  const ctx=vm.createContext({cursorSnapshot:snapshot,observedPointer:{x:110,y:70},
    surface:{windows:new Map([[42,canvas]]),geometry:new Map([[42,geometry]])}});
  vm.runInContext(publish,ctx);
  return {ctx,geometry,snapshot};
}
test('minimizing preserves a genuine point so restore messages remain serviceable',()=>{
  const {ctx,geometry,snapshot}=fixture({x:512,y:384});
  vm.runInContext('publishPointer()',ctx);
  assert.deepEqual(snapshot.read(),{x:512,y:384});
  geometry.minimized=false;
  vm.runInContext('publishPointer()',ctx);
  assert.deepEqual({...snapshot.read()},{x:216,y:139});
});
test('a minimized window never invents an initial pointer location',()=>{
  const {ctx,snapshot}=fixture(null);
  vm.runInContext('publishPointer()',ctx);
  assert.equal(snapshot.read(),null);
});
