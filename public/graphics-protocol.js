import {resolveBlend} from './graphics-blend.js';
import {samplerDescriptor, validateAlphaTest, validateScissor, validateStages, DEPTH_COMPARES} from './d3d9-gpu.js';
/* Bounded graphics worker wire contract. State publication is release/acquire
 * through Atomics: payload/header fields are written before nonzero STATUS. */
export const GRAPHICS_LIMITS = Object.freeze({headerBytes: 64, maxResponseBytes: 64 * 1024 * 1024, maxUploadBytes: 32 * 1024 * 1024, maxBatchCommands: 512, maxDimension: 4096, maxTimeoutMs: 60000});
export const HEADER = Object.freeze({status: 0, serial: 1, kind: 2, length: 3, width: 4, height: 5, transportFailure: 6});
export const VERTEX_FORMATS = Object.freeze(['XYZ_DIFFUSE', 'XYZ_DIFFUSE_UV', 'XYZRHW_DIFFUSE_UV', 'XYZ_DIFFUSE_UV2', 'XYZRHW_DIFFUSE_UV2']);
export const COMMANDS = new Set(['setRenderTargetFormat', 'createRenderTarget', 'setRenderTarget', 'readRenderTarget', 'writeRenderTarget', 'copyRenderTarget', 'clear', 'uploadVertices', 'uploadIndices', 'uploadTextureRGBA', 'drawIndexed', 'release', 'readPixels', 'present', 'validate', 'batch', 'dispose']);
const object = x => x !== null && typeof x === 'object' && !Array.isArray(x);
function fields(value, allowed) { if (!object(value) || Object.keys(value).some(k => !allowed.includes(k))) throw new TypeError('Invalid graphics command fields'); }
function integer(value, min, max, name) { if (!Number.isSafeInteger(value) || value < min || value > max) throw new RangeError(`Invalid ${name}`); }
export function validateCommand(command, args = {}, inBatch = false) {
  if (!COMMANDS.has(command)) throw new Error(`Unsupported graphics command ${command}`);
  if (!object(args)) throw new TypeError('Graphics args must be an object');
  let uploadBytes = 0;
  if (command === 'batch') {
    if (inBatch) throw new Error('Nested graphics batches unsupported');
    fields(args, ['commands']);
    if (!Array.isArray(args.commands)) throw new TypeError('Expected command batch');
    integer(args.commands.length, 1, GRAPHICS_LIMITS.maxBatchCommands, 'batch size');
    for (const entry of args.commands) {
      fields(entry, ['command', 'args']);
      if (['init', 'dispose', 'present', 'readPixels', 'readRenderTarget', 'validate'].includes(entry.command)) throw new Error(`Command ${entry.command} not allowed in batch`);
      uploadBytes += validateCommand(entry.command, entry.args ?? {}, true);
    }
  } else if (command.startsWith('upload')) {
    fields(args, command === 'uploadTextureRGBA' ? ['bytes', 'width', 'height'] : ['bytes', 'format']);
    if (!(args.bytes instanceof ArrayBuffer)) throw new TypeError('Upload requires owned ArrayBuffer');
    uploadBytes = args.bytes.byteLength;
    integer(uploadBytes, 1, GRAPHICS_LIMITS.maxUploadBytes, 'upload bytes');
    if (command === 'uploadTextureRGBA') {
      integer(args.width, 1, GRAPHICS_LIMITS.maxDimension, 'texture width'); integer(args.height, 1, GRAPHICS_LIMITS.maxDimension, 'texture height');
      if (uploadBytes !== args.width * args.height * 4) throw new RangeError('Texture bytes do not match dimensions');
    } else if (command === 'uploadVertices' && !VERTEX_FORMATS.includes(args.format)) throw new Error('Unsupported vertex format');
    else if (command === 'uploadIndices' && !['uint16', 'uint32'].includes(args.format)) throw new Error('Unsupported index format');
  } else if (command === 'drawIndexed') {
    fields(args, ['vertices', 'indices', 'firstIndex', 'indexCount', 'baseVertex', 'world', 'view', 'projection', 'texture', 'depthTest', 'depthCompare', 'depthWrite', 'blend', 'blendMode', 'blendFactors', 'scissor', 'viewport', 'sampler', 'alphaTest', 'textureEquation', 'textureFactor', 'stages', 'texture1', 'sampler1', 'depthBias', 'slopeScaleDepthBias', 'colorWriteMask', 'blendConstant']);
    for (const key of ['depthBias', 'slopeScaleDepthBias']) if (args[key] !== undefined && !Number.isFinite(args[key])) throw new TypeError(`Invalid ${key}`);
    if (args.colorWriteMask !== undefined) integer(args.colorWriteMask, 0, 15, 'colour write mask');
    if (args.blendConstant !== undefined) integer(args.blendConstant, 0, 0xffffffff, 'blend constant');
    if (args.texture1 != null) integer(args.texture1, 1, 0x7fffffff, 'texture1');
    if (args.sampler1 !== undefined) samplerDescriptor(args.sampler1);
    if (args.stages !== undefined) validateStages(args.stages);
    if(args.textureEquation!==undefined&&!['modulate','factor-rgb-texture-alpha','diffuse','diffuse-rgb-texture-alpha'].includes(args.textureEquation))throw new TypeError('Unsupported texture equation');
    if(args.textureEquation==='factor-rgb-texture-alpha'&&!args.texture)throw new TypeError('Factor texture equation requires texture');
    resolveBlend(args);
    if(args.textureEquation==='diffuse'&&args.texture)throw new TypeError('Diffuse equation forbids texture');
    if(args.textureEquation==='diffuse-rgb-texture-alpha'&&!args.texture)throw new TypeError('Diffuse RGB texture alpha requires texture');
    if(args.textureFactor!==undefined)integer(args.textureFactor,0,0xffffffff,'texture factor');
    if(args.alphaTest!==undefined)validateAlphaTest(args.alphaTest);
    if(args.sampler!==undefined)samplerDescriptor(args.sampler);
    for (const key of ['vertices', 'indices']) integer(args[key], 1, 0x7fffffff, key);
    if (args.texture != null) integer(args.texture, 1, 0x7fffffff, 'texture');
    for (const key of ['world', 'view', 'projection']) if (args[key] !== undefined && ((!Array.isArray(args[key]) && !(args[key] instanceof Float32Array)) || args[key].length !== 16 || Array.from(args[key]).some(x => !Number.isFinite(x)))) throw new TypeError('Invalid transform');
    for (const key of ['firstIndex', 'indexCount']) if (args[key] !== undefined) integer(args[key], key === 'indexCount' ? 1 : 0, 0xffffffff, key);
    if (args.baseVertex !== undefined) integer(args.baseVertex, -0x80000000, 0x7fffffff, 'base vertex');
    if(args.scissor!==undefined)validateScissor(args.scissor,GRAPHICS_LIMITS.maxDimension,GRAPHICS_LIMITS.maxDimension);
    if(args.blend!==undefined&&typeof args.blend!=='boolean')throw new TypeError('Invalid blend enable');
    if(args.depthCompare!==undefined&&!DEPTH_COMPARES.includes(args.depthCompare))throw new TypeError('Unsupported depth comparison');
    if(args.depthWrite!==undefined&&typeof args.depthWrite!=='boolean')throw new TypeError('Invalid depth write');
    if (args.depthTest !== undefined && typeof args.depthTest !== 'boolean') throw new TypeError('Invalid depth test');
    if (args.viewport != null) {
      fields(args.viewport, ['x', 'y', 'width', 'height', 'minDepth', 'maxDepth']);
      for (const key of ['x', 'y', 'width', 'height']) if (!Number.isFinite(args.viewport[key])) throw new TypeError('Invalid viewport');
    }
  } else if (command === 'createRenderTarget') {
    fields(args, ['width', 'height']); integer(args.width, 1, GRAPHICS_LIMITS.maxDimension, 'render target width'); integer(args.height, 1, GRAPHICS_LIMITS.maxDimension, 'render target height');
  } else if (command === 'copyRenderTarget') {
    fields(args, ['source', 'destination', 'region']); integer(args.source, 1, 0x7fffffff, 'source render target'); integer(args.destination, 1, 0x7fffffff, 'destination render target');
    if (args.region !== undefined) {
      fields(args.region, ['sourceX', 'sourceY', 'x', 'y', 'width', 'height']);
      for (const key of ['sourceX', 'sourceY', 'x', 'y']) integer(args.region[key], 0, GRAPHICS_LIMITS.maxDimension - 1, `copy ${key}`);
      for (const key of ['width', 'height']) integer(args.region[key], 1, GRAPHICS_LIMITS.maxDimension, `copy ${key}`);
    }
    if (args.source === args.destination) throw new RangeError('Render target copy requires distinct targets');
  } else if (command === 'readRenderTarget') {
    fields(args, ['texture']); integer(args.texture, 1, 0x7fffffff, 'render target texture');
  } else if (command === 'writeRenderTarget') {
    fields(args, ['texture', 'width', 'height', 'bytes']); integer(args.texture, 1, 0x7fffffff, 'render target texture');
    integer(args.width, 1, GRAPHICS_LIMITS.maxDimension, 'render target width'); integer(args.height, 1, GRAPHICS_LIMITS.maxDimension, 'render target height');
    if (!(args.bytes instanceof ArrayBuffer)) throw new TypeError('Upload requires owned ArrayBuffer');
    uploadBytes = args.bytes.byteLength; if (uploadBytes !== args.width * args.height * 4) throw new RangeError('Render target bytes do not match dimensions');
  } else if (command === 'setRenderTarget') {
    fields(args, ['texture']); if (args.texture !== null) integer(args.texture, 1, 0x7fffffff, 'render target texture');
  } else if (command === 'setRenderTargetFormat') {
    fields(args, ['format']); if (![22,25].includes(args.format)) throw new RangeError('Unsupported logical target format');
  } else if (command === 'clear') {
    fields(args, ['color', 'depth', 'scissor', 'zbuffer', 'target']);
    if (args.zbuffer !== undefined && typeof args.zbuffer !== 'boolean') throw new TypeError('Invalid clear zbuffer flag');
    if (args.target !== undefined && typeof args.target !== 'boolean') throw new TypeError('Invalid clear target flag');
    if(args.scissor!==undefined)validateScissor(args.scissor,GRAPHICS_LIMITS.maxDimension,GRAPHICS_LIMITS.maxDimension);
    if (args.color !== undefined) integer(args.color, 0, 0xffffffff, 'clear color');
    if (args.depth !== undefined && (!Number.isFinite(args.depth) || args.depth < 0 || args.depth > 1)) throw new RangeError('Invalid clear depth');
  } else if (command === 'release') { fields(args, ['handle']); integer(args.handle, 1, 0x7fffffff, 'resource handle'); }
  else fields(args, []);
  if (uploadBytes > GRAPHICS_LIMITS.maxUploadBytes) throw new RangeError('Batch upload budget exceeded');
  return uploadBytes;
}
export function responseViews(buffer) {
  if (!(buffer instanceof SharedArrayBuffer) || buffer.byteLength < GRAPHICS_LIMITS.headerBytes + 1024 || buffer.byteLength > GRAPHICS_LIMITS.headerBytes + GRAPHICS_LIMITS.maxResponseBytes) throw new RangeError('Invalid shared response capacity');
  return {header: new Int32Array(buffer, 0, GRAPHICS_LIMITS.headerBytes / 4), payload: new Uint8Array(buffer, GRAPHICS_LIMITS.headerBytes)};
}
export function publishResponse(views, serial, result, error = false) {
  integer(serial, 1, 0x7fffffff, 'response serial');
  let bytes, kind = 0, width = 0, height = 0;
  if (!error && result?.rgba instanceof Uint8Array) {
    kind = 1; width = result.width; height = result.height; bytes = result.rgba;
    integer(width, 1, GRAPHICS_LIMITS.maxDimension, 'readback width'); integer(height, 1, GRAPHICS_LIMITS.maxDimension, 'readback height');
    if (bytes.length !== width * height * 4) throw new RangeError('Invalid readback payload');
  } else bytes = new TextEncoder().encode(JSON.stringify(result ?? null));
  if (bytes.length > views.payload.length) throw new RangeError('Graphics response capacity exceeded');
  views.payload.set(bytes); Atomics.store(views.header, HEADER.serial, serial); Atomics.store(views.header, HEADER.kind, kind);
  Atomics.store(views.header, HEADER.length, bytes.length); Atomics.store(views.header, HEADER.width, width); Atomics.store(views.header, HEADER.height, height);
  Atomics.store(views.header, HEADER.status, error ? 2 : 1); Atomics.notify(views.header, HEADER.status);
}
export function consumeResponse(views, serial) {
  const state = Atomics.load(views.header, HEADER.status), replySerial = Atomics.load(views.header, HEADER.serial);
  if (replySerial !== serial || ![1, 2].includes(state)) throw new Error('Graphics response serial/status mismatch');
  const length = Atomics.load(views.header, HEADER.length), kind = Atomics.load(views.header, HEADER.kind);
  integer(length, 0, views.payload.length, 'response length');
  const bytes = views.payload.slice(0, length);
  if (state === 2) { const detail = JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(bytes)); throw new Error(`Graphics worker: ${detail.error}`); }
  if (kind === 0) return JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(bytes));
  if (kind !== 1) throw new Error('Invalid graphics response kind');
  const width = Atomics.load(views.header, HEADER.width), height = Atomics.load(views.header, HEADER.height);
  integer(width, 1, GRAPHICS_LIMITS.maxDimension, 'readback width'); integer(height, 1, GRAPHICS_LIMITS.maxDimension, 'readback height');
  if (length !== width * height * 4) throw new Error('Invalid readback response size');
  return {width, height, rgba: bytes};
}
