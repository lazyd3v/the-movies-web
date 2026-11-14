import test from 'node:test';
import assert from 'node:assert/strict';
import {MoviesD3D9GPU, matrixBytes, rgbaFromD3DColor, validateVertexBytes, transformedW, vertexLayout, uvOffset, uv1Offset, validateIndexBytes, validateIndexedRange} from '../public/d3d9-gpu.js';
test('D3D color and matrix validation preserve byte ordering and finite32 bounds', () => {
  assert.deepEqual(rgbaFromD3DColor(0x80402010), [64/255, 32/255, 16/255, 128/255]);
  assert.equal(matrixBytes()[15], 1);
  const rowMajor = [1,0,0,0, 0,1,0,0, 0,0,1,0, .5,.25,0,1];
  assert.deepEqual(Array.from(matrixBytes(rowMajor)), rowMajor);
  for (const bad of [[], new Array(16).fill(NaN), new Array(16).fill(1e100)]) assert.throws(() => matrixBytes(bad));
  assert.throws(() => rgbaFromD3DColor(-1));
});
test('vertex upload copies owned data; rejects unimplemented formats and malformed bytes', () => {
  const bytes = new ArrayBuffer(48); new DataView(bytes).setFloat32(0, .25, true);
  const vertices = validateVertexBytes(bytes, 'XYZ_DIFFUSE');
  new DataView(bytes).setFloat32(0, 99, true);
  assert.equal(new DataView(vertices.bytes.buffer).getFloat32(0, true), .25);
  assert.equal(vertices.count, 3);
  assert.equal(validateVertexBytes(new Uint8Array(72), 'XYZ_DIFFUSE_UV').stride, 24);
  assert.throws(() => validateVertexBytes(bytes, 'XYZRHW_DIFFUSE'));
  assert.throws(() => validateVertexBytes(new Uint8Array(47), 'XYZ_DIFFUSE'));
  new DataView(bytes).setFloat32(0, Infinity, true);
  assert.throws(() => validateVertexBytes(bytes, 'XYZ_DIFFUSE'));
});
test('indexed triangle range honors index width, start and signed base vertex', () => {
  const vertices = {count: 3};
  const indices = validateIndexBytes(new Uint16Array([99, 1, 2, 3]), 'uint16');
  validateIndexedRange(vertices, indices, 1, 3, -1);
  assert.throws(() => validateIndexedRange(vertices, indices, 0, 3, 0));
  assert.throws(() => validateIndexedRange(vertices, indices, 1, 3, 0));
  assert.throws(() => validateIndexedRange(vertices, indices, 1, 2, -1));
  assert.throws(() => validateIndexedRange(vertices, indices, 2, 3, -1));
  const high = validateIndexBytes(new Uint32Array([0x80000000, 0x80000001, 0x80000002]), 'uint32');
  validateIndexedRange(vertices, high, 0, 3, -0x80000000);
  assert.throws(() => validateIndexBytes(new Uint8Array(3), 'uint16'));
});
test('no GPU availability never reports synthetic device success', async () => {
  await assert.rejects(MoviesD3D9GPU.create({gpu: null}), /required/);
  await assert.rejects(MoviesD3D9GPU.create({gpu: {requestAdapter: async () => null}, canvas: {getContext(){}, transferToImageBitmap(){}}}), /No WebGPU adapter/);
});

test('logical A1R5G5B5 quantizes every channel and preserves only32 RGB levels', async () => {
  const {quantizeA1R5G5B5} = await import('../public/d3d9-gpu.js');
  assert.deepEqual(quantizeA1R5G5B5([17/255,34/255,51/255,.49]),[2/31,4/31,6/31,0]);
  assert.deepEqual(quantizeA1R5G5B5([-1,2,0,.5]),[0,1,0,1]);
  const levels=new Set();for(let x=0;x<256;x++)levels.add(quantizeA1R5G5B5([x/255,0,0,1])[0]);assert.equal(levels.size,32);
  assert.throws(()=>quantizeA1R5G5B5([0,NaN,0,1]));
});

test('sampler state maps actual point/linear and wrap/mirror/clamp, only one mip', async () => {
  const {samplerDescriptor} = await import('../public/d3d9-gpu.js');
  const result=samplerDescriptor({addressU:1,addressV:2,addressW:3,minFilter:2,magFilter:2,mipFilter:1,lodBias:-1});
  assert.deepEqual(result.descriptor,{addressModeU:'repeat',addressModeV:'mirror-repeat',addressModeW:'clamp-to-edge',minFilter:'linear',magFilter:'linear',mipmapFilter:'nearest',lodMinClamp:0,lodMaxClamp:0});
  assert.equal(result.state.lodBias,-1);
  for(const value of [{addressU:4},{minFilter:3},{mipFilter:3},{lodBias:NaN},{lodBias:17},{unknown:1}])assert.throws(()=>samplerDescriptor(value));
});

test('alpha test accepts only explicit comparisons and8-bit reference', async () => {
 const {validateAlphaTest}=await import('../public/d3d9-gpu.js');
 for(let func=1;func<=8;func++)assert.deepEqual(validateAlphaTest({enabled:true,func,reference:100}),{enabled:true,func,reference:100});
 for(const state of [{func:0},{func:9},{reference:256},{reference:-1},{enabled:1},{unknown:1}])assert.throws(()=>validateAlphaTest(state));
});

test('XYZRHW preserves exact 28-byte source; any finite depth and RHW are accepted',()=>{
 const bytes=new ArrayBuffer(28),view=new DataView(bytes);
 [19.5,23.5,.5,.5].forEach((v,i)=>view.setFloat32(i*4,v,true));view.setUint32(16,0x7f123456,true);view.setFloat32(20,.25,true);view.setFloat32(24,.75,true);
 const vertex=validateVertexBytes(bytes,'XYZRHW_DIFFUSE_UV');assert.equal(vertex.stride,28);assert.equal(vertex.count,1);assert.deepEqual(vertex.bytes,new Uint8Array(bytes));
 for(const bad of [Infinity,NaN]){view.setFloat32(12,bad,true);assert.throws(()=>validateVertexBytes(bytes,'XYZRHW_DIFFUSE_UV'),/Nonfinite/);}
 // Legal in D3D9 (Finances screen): RHW 0/negative/denormal and depth outside [0,1].
 for(const rhw of [0,-1,1e-45]){view.setFloat32(12,rhw,true);assert.equal(validateVertexBytes(bytes,'XYZRHW_DIFFUSE_UV').count,1);assert.equal(transformedW(Math.fround(rhw)),1);}view.setFloat32(12,.5,true);
 for(const z of [-.1,1.1]){view.setFloat32(8,z,true);assert.equal(validateVertexBytes(bytes,'XYZRHW_DIFFUSE_UV').count,1);}view.setFloat32(8,.5,true);
 assert.equal(transformedW(.5),2);
 assert.throws(()=>validateVertexBytes(new Uint8Array(27),'XYZRHW_DIFFUSE_UV'),/complete/);
});
test('raw FVF layouts read the game\'s vertices in place and zero non-finite fields', () => {
  const layout = vertexLayout('raw/40/3/12/-/24/32');
  assert.deepEqual([layout.stride, layout.rhw, layout.uvSets, layout.normal, layout.noDiffuse, uvOffset(layout), uv1Offset(layout)], [40, false, 2, true, true, 24, 32]);
  const bytes = new ArrayBuffer(80), f = new Float32Array(bytes);
  f.set([1, 2, 3, 0, 0, 1, 7, 7, .5, .25, 9, 9]); f[10 + 0] = NaN; f[10 + 2] = Infinity; f[10 + 6] = NaN; // vertex 1: x, z and u0 broken
  const vertex = validateVertexBytes(bytes, 'raw/40/3/12/-/24/32', true);
  assert.equal(vertex.count, 2); const out = new Float32Array(vertex.bytes.buffer, vertex.bytes.byteOffset, 20);
  assert.deepEqual([out[10], out[12], out[16]], [0, 0, 0]); assert.equal(out[0], 1);
  assert.equal(vertexLayout('raw/24/4/-/-/16/-').rhw, true);
  for (const bad of ['raw/40/3/12/-/36/-', 'raw/40/5/-/-/24/-', 'raw/40/3/8/-/24/-', 'raw/41/3/-/-/24/-', 'raw/40/3/-/-/-/32']) assert.throws(() => vertexLayout(bad), /Unsupported vertex format/, bad);
  assert.throws(() => validateVertexBytes(new Float32Array([NaN, 0, 0, 0]).buffer, 'XYZ_DIFFUSE'), /Nonfinite/, 'named layouts still reject');
});
