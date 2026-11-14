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
  assert.equal(host.liveAnnotations(), 1);
  call('release',{handle:vertices}); call('release',{handle:indices});
  assert.equal(host.liveAnnotations(), 0, 'released resources leave the trace annotations');
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
test('D3D render-target textures are typed, switchable and sampleable only through host ownership', () => {
  let handle = 0;
  const {host, calls, device} = fixture({call(command, args) {
    calls.push({command, args});
    if (command.startsWith('upload') || command === 'createRenderTarget') return ++handle;
    if (command === 'setRenderTargetFormat') return args.format;
    if (command === 'setRenderTarget') return args.texture === null ? {width:1024, height:768} : {width:512, height:512};
    return null;
  }});
  const call = host.moviesGraphicsCall;
  call('createDevice', device);
  assert.throws(() => call('createRenderTarget', {width:9000, height:512}), /exceeds actual device limit|Invalid render target/);
  const target = call('createRenderTarget', {width:512, height:512});
  const texture = call('uploadTextureRGBA', {bytes:new ArrayBuffer(16),width:2,height:2});
  assert.throws(() => call('setRenderTarget', {texture}), /does not belong/);
  assert.throws(() => call('setRenderTarget', {texture:999}), /does not belong/);
  assert.deepEqual(call('setRenderTarget', {texture:target}), {width:512, height:512});
  assert.deepEqual(call('setRenderTarget', {texture:null}), {width:1024, height:768});
  const vertices = call('uploadVertices', {bytes:new ArrayBuffer(72),format:'XYZ_DIFFUSE_UV'});
  const indices = call('uploadIndices', {bytes:new ArrayBuffer(6),format:'uint16'});
  call('drawIndexed', {vertices,indices,texture:target,indexCount:3});
  call('release', {handle:target});
  assert.throws(() => call('setRenderTarget', {texture:target}), /does not belong/);
  assert.ok(calls.some(c => c.command === 'createRenderTarget' && c.args.width === 512));
});
test('deferred D3D host batches fire-and-forget commands and flushes before present', () => {
  const calls = []; let handle = 0;
  const client = {capabilities: {backend: 'WebGPU', width: 1024, height: 768, depthFormat: 'depth16unorm', renderTargetFormats:[22,25], limits: {maxBufferSize: 256 * 1024 * 1024, maxTextureDimension2D:8192}},
    call(command, args) {
      calls.push(command);
      if (command === 'batch') return args.commands.map(e => e.command.startsWith('upload') || e.command === 'createRenderTarget' ? ++handle : e.command === 'setRenderTarget' ? {width:1, height:1} : null);
      return command === 'setRenderTargetFormat' ? args.format : command === 'present' ? {acknowledged: true, serial: 1} : null;
    }, close() {}};
  const host = createMoviesD3D9Host({client, getWindow: () => ({clientWidth:1024, clientHeight:768}), presentationIntervalMask:1, deferred:true});
  const call = host.moviesGraphicsCall;
  call('createDevice', {width:1024,height:768,hwnd:7,depth16:true,windowed:true,presentationInterval:0});
  calls.length = 0;
  const vertices = call('uploadVertices', {bytes:new ArrayBuffer(48),format:'XYZ_DIFFUSE'});
  const indices = call('uploadIndices', {bytes:new ArrayBuffer(6),format:'uint16'});
  const target = call('createRenderTarget', {width:256, height:128});
  assert.deepEqual([vertices, indices, target], [1, 2, 3]);
  assert.deepEqual(call('setRenderTarget', {texture:target}), {width:256, height:128});
  assert.deepEqual(call('setRenderTarget', {texture:null}), {width:1024, height:768});
  call('clear', {color:0}); call('drawIndexed', {vertices, indices, indexCount:3}); call('release', {handle:target});
  assert.deepEqual(calls, [], 'nothing crosses the bridge before a synchronous command');
  assert.equal(call('present').acknowledged, true);
  assert.deepEqual(calls, ['batch', 'present'], 'one batch round trip, then present');
  calls.length = 0; assert.equal(call('present').acknowledged, true); assert.deepEqual(calls, ['present'], 'empty queue sends no batch');
});
test('deferred D3D host detects a handle the GPU worker did not assign as predicted', () => {
  const client = {capabilities: {backend: 'WebGPU', width: 1024, height: 768, depthFormat: 'depth16unorm', renderTargetFormats:[22], limits: {maxBufferSize: 1 << 28, maxTextureDimension2D:8192}},
    call(command, args) { return command === 'batch' ? args.commands.map(() => 42) : command === 'setRenderTargetFormat' ? args.format : command === 'present' ? {acknowledged: true, serial: 1} : null; }, close() {}};
  const host = createMoviesD3D9Host({client, getWindow: () => ({clientWidth:1024, clientHeight:768}), presentationIntervalMask:1, deferred:true});
  host.moviesGraphicsCall('createDevice', {width:1024,height:768,hwnd:7,depth16:true,windowed:true,presentationInterval:0});
  host.moviesGraphicsCall('uploadIndices', {bytes:new ArrayBuffer(6),format:'uint16'});
  assert.throws(() => host.moviesGraphicsCall('present'), /expected 1/);
});
test('deferred D3D host flushes when the batch command limit fills', () => {
  const batches = [];
  const client = {capabilities: {backend: 'WebGPU', width: 1024, height: 768, depthFormat: 'depth16unorm', renderTargetFormats:[22], limits: {maxBufferSize: 1 << 28, maxTextureDimension2D:8192}},
    call(command, args) { if (command === 'batch') { batches.push(args.commands.length); return args.commands.map(() => null); } return command === 'setRenderTargetFormat' ? args.format : command === 'present' ? {acknowledged: true, serial: 1} : null; }, close() {}};
  const host = createMoviesD3D9Host({client, getWindow: () => ({clientWidth:1024, clientHeight:768}), presentationIntervalMask:1, deferred:true});
  host.moviesGraphicsCall('createDevice', {width:1024,height:768,hwnd:7,depth16:true,windowed:true,presentationInterval:0});
  for (let i = 0; i < 1100; i++) host.moviesGraphicsCall('clear', {color:i});
  host.moviesGraphicsCall('present');
  assert.deepEqual(batches, [512, 512, 76]);
});
test('pipelined D3D host posts batches and surfaces handle mismatches at the next synchronous command', () => {
  const posted = []; let pending = null;
  let presents = 0, ackPresents = true;
  const settle = () => { const p = pending; pending = null; p?.check(p.args.commands.map(c => c.command === 'present' ? {acknowledged: ackPresents, serial: ++presents} : 42)); };
  const client = {capabilities: {backend: 'WebGPU', width: 1024, height: 768, depthFormat: 'depth16unorm', renderTargetFormats:[22], limits: {maxBufferSize: 1 << 28, maxTextureDimension2D:8192}},
    post(command, args, check) { settle(); posted.push(command); pending = {args, check}; },
    call(command, args) { settle(); return command === 'setRenderTargetFormat' ? args.format : command === 'present' ? {acknowledged: true, serial: 1} : null; }, close() {}};
  const host = createMoviesD3D9Host({client, getWindow: () => ({clientWidth:1024, clientHeight:768}), presentationIntervalMask:1, deferred:true, pipelined:true});
  host.moviesGraphicsCall('createDevice', {width:1024,height:768,hwnd:7,depth16:true,windowed:true,presentationInterval:0});
  host.moviesGraphicsCall('clear', {color:0});
  const first = host.moviesGraphicsCall('present');
  assert.deepEqual([first.acknowledged, first.serial], [true, 1]);
  assert.deepEqual(posted, ['batch'], 'present closes the frame batch instead of a synchronous call');
  host.moviesGraphicsCall('uploadIndices', {bytes:new ArrayBuffer(6),format:'uint16'});
  host.moviesGraphicsCall('present'); // the bad handle is in flight with this frame
  assert.throws(() => host.moviesGraphicsCall('present'), /expected 1/, 'surfaced at the next exchange');
});
test('pipelined presents are verified one frame later', () => {
  const posted = []; let pending = null, presents = 0, ack = true;
  const settle = () => { const p = pending; pending = null; p?.check(p.args.commands.map(c => c.command === 'present' ? {acknowledged: ack, serial: ++presents} : null)); };
  const client = {capabilities: {backend: 'WebGPU', width: 1024, height: 768, depthFormat: 'depth16unorm', renderTargetFormats:[22], limits: {maxBufferSize: 1 << 28, maxTextureDimension2D:8192}},
    post(command, args, check) { settle(); posted.push(args.commands.map(c => c.command).join(',')); pending = {args, check}; },
    call(command, args) { settle(); return command === 'setRenderTargetFormat' ? args.format : null; }, close() {}};
  const host = createMoviesD3D9Host({client, getWindow: () => ({clientWidth:1024, clientHeight:768}), presentationIntervalMask:1, deferred:true, pipelined:true});
  host.moviesGraphicsCall('createDevice', {width:1024,height:768,hwnd:7,depth16:true,windowed:true,presentationInterval:0});
  for (let frame = 1; frame <= 3; frame++) { host.moviesGraphicsCall('clear', {color:0}); assert.equal(host.moviesGraphicsCall('present').serial, frame); }
  assert.deepEqual(posted, ['clear,present', 'clear,present', 'clear,present']);
  ack = false; host.moviesGraphicsCall('clear', {color:0});
  assert.throws(() => host.moviesGraphicsCall('present'), /not acknowledged/);
});
