import test from 'node:test';
import assert from 'node:assert/strict';
import {createMoviesD3D9Host} from '../public/d3d9-host.js';

function fixture(overrides = {}) {
  const calls = []; let handle = 0;
  const client = {capabilities: {backend: 'WebGPU', width: 1024, height: 768,
    depthFormat: 'depth16unorm', renderTargetFormats:[22,25], limits: {maxBufferSize: 256 * 1024 * 1024, maxTextureDimension2D:8192}},
    call(command, args) { calls.push({command, args}); return command.startsWith('upload') ? ++handle : command === 'setRenderTargetFormat' ? args.format : command === 'present' ? {acknowledged: true, serial: 1} : null; }, close() { calls.push({command:'close'}); }, ...overrides};
  const host = createMoviesD3D9Host({client, getWindow: hwnd => hwnd === 7 ? {clientWidth:1024, clientHeight:768} : null, presentationIntervalMask:1});
  const device = {width:1024,height:768,hwnd:7,depth16:true,windowed:true,presentationInterval:0};
  return {host, calls, device};
}
test('D3D host validates actual window, dimensions, pacing and GPU before device creation', () => {
  const {host, calls, device} = fixture(), call = host.moviesGraphicsCall;
  assert.equal(call('capabilities').maxBufferBytes, 32 * 1024 * 1024);
  assert.throws(() => call('present'), /not been created/);
  for (const change of [{hwnd:8},{width:640},{presentationInterval:0x80000000},{depth16:1}]) assert.throws(() => call('createDevice', {...device,...change}));
  assert.equal(calls.length, 0);
  assert.equal(call('createDevice',device), true);
  assert.equal(calls[0].command,'validate');
  assert.throws(() => call('createDevice',device), /Only one/);
});
test('D3D resource lifetime and acknowledged presentation survive through transport', () => {
  const {host, calls, device} = fixture(), call = host.moviesGraphicsCall;
  call('createDevice',device);
  const vertices = call('uploadVertices',{bytes:new ArrayBuffer(48),format:'XYZ_DIFFUSE'});
  const indices = call('uploadIndices',{bytes:new ArrayBuffer(6),format:'uint16'});
  call('drawIndexed',{vertices,indices,indexCount:3});
  assert.equal(call('present').acknowledged,true);
  assert.throws(() => call('destroyDevice'), /Live GPU/);
  assert.throws(() => call('release',{handle:99}), /does not belong/);
  call('release',{handle:vertices}); call('release',{handle:indices});
  call('destroyDevice'); assert.equal(calls.at(-1).command,'dispose');
  assert.throws(() => call('capabilities'), /closed/);
});
test('D3D host does not accept asynchronous or unacknowledged GPU operations', () => {
  const async = fixture({call:() => Promise.resolve(null)});
  assert.throws(() => async.host.moviesGraphicsCall('createDevice',async.device), /synchronously/);
  const failed = fixture({call:(command,args) => command === 'setRenderTargetFormat' ? args.format : null});
  failed.host.moviesGraphicsCall('createDevice',failed.device);
  assert.throws(() => failed.host.moviesGraphicsCall('present'), /not acknowledged/);
});
test('D3D device creation requires actual logical backbuffer acknowledgement', () => {
  const {host,device,calls} = fixture();
  assert.equal(host.moviesGraphicsCall('createDevice',{...device,backBufferFormat:25}),true);
  assert.deepEqual(calls.at(-1),{command:'setRenderTargetFormat',args:{format:25}});
  const rejected = fixture({call:() => null});
  assert.throws(() => rejected.host.moviesGraphicsCall('createDevice',rejected.device), /not acknowledged/);
});
test('D3D texture draw preserves typed resource ownership', () => {
  const {host, device} = fixture(), call = host.moviesGraphicsCall;
  call('createDevice', device);
  const vertices = call('uploadVertices', {bytes:new ArrayBuffer(72),format:'XYZ_DIFFUSE_UV'});
  const indices = call('uploadIndices', {bytes:new ArrayBuffer(6),format:'uint16'});
  const texture = call('uploadTextureRGBA', {bytes:new ArrayBuffer(16),width:2,height:2});
  assert.throws(() => call('drawIndexed', {vertices:indices,indices:vertices,texture,indexCount:3}), /Invalid native draw/);
  assert.throws(() => call('drawIndexed', {vertices,indices,texture:vertices,indexCount:3}), /Invalid native draw/);
  call('drawIndexed', {vertices,indices,texture,indexCount:3});
  call('release', {handle:texture});
  assert.throws(() => call('drawIndexed', {vertices,indices,texture,indexCount:3}), /Invalid native draw/);
});
