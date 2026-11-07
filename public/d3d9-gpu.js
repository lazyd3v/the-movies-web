import {resolveBlend, PipelineCache} from './graphics-blend.js';
/* Bounded D3D9 fixed-function graphics foundation, NOT a D3D9 implementation.
 * No upstream source copied. WebGPU/WGSL specification: https://gpuweb.github.io/gpuweb/
 * Async initialization and readback; synchronous command submission/presentation.
 */
export class GraphicsUnsupportedError extends Error {
  constructor(message) { super(message); this.name = 'GraphicsUnsupportedError'; }
}
const IDENTITY = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
export function checkedInteger(value, min, max, name) {
  if (!Number.isSafeInteger(value) || value < min || value > max) throw new RangeError(`Invalid ${name}`);
  return value;
}
export function matrixBytes(matrix = IDENTITY) {
  if (!matrix || matrix.length !== 16 || Array.from(matrix).some(x => !Number.isFinite(x))) throw new TypeError('Expected16 finite matrix elements');
  const result = Float32Array.from(matrix);
  if (result.some(x => !Number.isFinite(x))) throw new RangeError('Matrix cannot be represented as float32');
  return result;
}
export function rgbaFromD3DColor(color) {
  checkedInteger(color, 0, 0xffffffff, 'D3DCOLOR');
  return [((color >>> 16) & 255) / 255, ((color >>> 8) & 255) / 255, (color & 255) / 255, (color >>> 24) / 255];
}
function bytesOf(input) {
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength).slice();
  if (input instanceof ArrayBuffer) return new Uint8Array(input).slice();
  throw new TypeError('Expected ArrayBuffer or typed array');
}
export function validateVertexBytes(input, format) {
  const stride = format === 'XYZ_DIFFUSE' ? 16 : format === 'XYZ_DIFFUSE_UV' ? 24 : format === 'XYZRHW_DIFFUSE_UV' ? 28 : 0;
  if (!stride) throw new GraphicsUnsupportedError(`Unsupported vertex format ${format}`);
  const bytes = bytesOf(input);
  if (!bytes.length || bytes.length % stride) throw new RangeError('Vertex bytes must contain complete vertices');
  const view = new DataView(bytes.buffer);
  for (let offset = 0; offset < bytes.length; offset += stride) {
    for (const field of stride === 28 ? [0,4,8,12,20,24] : stride === 24 ? [0, 4, 8, 16, 20] : [0, 4, 8]) {
      if (!Number.isFinite(view.getFloat32(offset + field, true))) throw new RangeError('Nonfinite vertex component');
    }
  }
  if(stride===28)for(let offset=0;offset<bytes.length;offset+=stride){
    const z=view.getFloat32(offset+8,true),rhw=view.getFloat32(offset+12,true);
    if(z<0||z>1||rhw<=0||!Number.isFinite(Math.fround(1/rhw)))throw new RangeError('Invalid transformed depth or reciprocal W');
  }
  return {bytes, stride, count: bytes.length / stride, format};
}
export function validateIndexBytes(input, format) {
  const stride = format === 'uint16' ? 2 : format === 'uint32' ? 4 : 0;
  if (!stride) throw new GraphicsUnsupportedError(`Unsupported index format ${format}`);
  const bytes = bytesOf(input);
  if (!bytes.length || bytes.length % stride) throw new RangeError('Incomplete index buffer');
  return {bytes, stride, count: bytes.length / stride, format};
}
export function validateIndexedRange(vertices, indices, firstIndex, indexCount, baseVertex = 0) {
  checkedInteger(firstIndex, 0, indices.count, 'first index');
  checkedInteger(indexCount, 1, indices.count - firstIndex, 'index count');
  checkedInteger(baseVertex, -0x80000000, 0x7fffffff, 'base vertex');
  if (indexCount % 3) throw new GraphicsUnsupportedError('Only complete triangle lists supported');
  const v = new DataView(indices.bytes.buffer, indices.bytes.byteOffset, indices.bytes.byteLength);
  for (let i = firstIndex; i < firstIndex + indexCount; i++) {
    const index = indices.stride === 2 ? v.getUint16(i * 2, true) : v.getUint32(i * 4, true);
    if (index + baseVertex < 0 || index + baseVertex >= vertices.count) throw new RangeError('Indexed vertex outside buffer');
  }
}
export function quantizeA1R5G5B5(rgba) {
  if (!rgba || rgba.length !== 4 || Array.from(rgba).some(x => !Number.isFinite(x))) throw new TypeError('Expected finite RGBA');
  return Array.from(rgba, (x, i) => { const scale = i === 3 ? 1 : 31; return Math.floor(Math.min(1, Math.max(0, x)) * scale + .5) / scale; });
}
export function samplerDescriptor(input = {}) {
  const allowed=['addressU','addressV','addressW','minFilter','magFilter','mipFilter','lodBias'];
  if (!input || Object.keys(input).some(key=>!allowed.includes(key))) throw new GraphicsUnsupportedError('Unknown sampler field');
  const state={addressU:3,addressV:3,addressW:3,minFilter:1,magFilter:1,mipFilter:0,lodBias:0,...input};
  for(const key of ['addressU','addressV','addressW'])if(![1,2,3].includes(state[key]))throw new GraphicsUnsupportedError('Unsupported texture address mode');
  for(const key of ['minFilter','magFilter'])if(![1,2].includes(state[key]))throw new GraphicsUnsupportedError('Unsupported texture filter');
  if(![0,1,2].includes(state.mipFilter)||!Number.isFinite(state.lodBias)||state.lodBias < -16 || state.lodBias > 15.99)throw new GraphicsUnsupportedError('Unsupported mip filter/bias');
  const addresses={1:'repeat',2:'mirror-repeat',3:'clamp-to-edge'};
  return {state,descriptor:{addressModeU:addresses[state.addressU],addressModeV:addresses[state.addressV],addressModeW:addresses[state.addressW],minFilter:state.minFilter===1?'nearest':'linear',magFilter:state.magFilter===1?'nearest':'linear',mipmapFilter:state.mipFilter===2?'linear':'nearest',lodMinClamp:0,lodMaxClamp:0}};
}
export function validateAlphaTest(input = {}) {
 if(!input||Object.keys(input).some(k=>!['enabled','func','reference'].includes(k)))throw new GraphicsUnsupportedError('Unsupported alpha-test field');
 const state={enabled:false,func:8,reference:0,...input};
 if(typeof state.enabled!=='boolean'||!Number.isInteger(state.func)||state.func<1||state.func>8||!Number.isInteger(state.reference)||state.reference<0||state.reference>255)throw new RangeError('Invalid alpha-test state');
 return state;
}
export function validateScissor(scissor,width,height) {
  if(scissor===null||scissor===undefined)return null;
  if(typeof scissor!=='object'||Object.keys(scissor).some(k=>!['x','y','width','height'].includes(k)))throw new TypeError('Invalid scissor fields');
  for(const key of ['x','y','width','height'])checkedInteger(scissor[key],0,0xffffffff,'scissor '+key);
  if(scissor.x+scissor.width>width||scissor.y+scissor.height>height)throw new RangeError('Scissor exceeds render target');
  return {...scissor};
}
function shader(uv, logicalFormat, rhw = false, blend = false, diffuseOnly = false) {
  return `struct Matrices { world: mat4x4<f32>, view: mat4x4<f32>, projection: mat4x4<f32>, lodBias: f32, alphaEnable: f32, alphaFunc: f32, alphaRef: f32, viewport: vec4<f32>, textureFactor: vec4<f32>, textureEquation: vec4<f32> };
@group(0) @binding(0) var<uniform> matrices: Matrices;
${uv && !diffuseOnly ? '@group(1) @binding(0) var texture0: texture_2d<f32>;\n@group(1) @binding(1) var sampler0: sampler;' : ''}
struct VertexOutput { @builtin(position) position: vec4<f32>, @location(0) ${rhw ? '@interpolate(linear)' : ''} color: vec4<f32>, ${uv ? '@location(1) uv: vec2<f32>,' : ''} };
@vertex fn vs(@location(0) position: ${rhw ? 'vec4' : 'vec3'}<f32>, @location(1) diffuse: u32 ${uv ? ', @location(2) uv: vec2<f32>' : ''}) -> VertexOutput {
 var out: VertexOutput;
 // Raw D3D row-major matrices become transposed WGSL column-major matrices.
 // P * V * W * column(position) equals D3D row(position) * W * V * P.
 ${rhw ? `// D3D9 integer pixel centers become WebGPU half-integer centers.
 let screen=vec2<f32>(2.0,-2.0)*(position.xy-matrices.viewport.xy+vec2<f32>(0.5))/matrices.viewport.zw+vec2<f32>(-1.0,1.0);
 out.position=vec4<f32>(screen,position.z,1.0)/position.w;` : 'out.position = matrices.projection * matrices.view * matrices.world * vec4<f32>(position, 1.0);'}
 out.color = vec4<f32>(f32((diffuse >> 16u) & 255u), f32((diffuse >> 8u) & 255u), f32(diffuse & 255u), f32(diffuse >> 24u)) / 255.0;
 ${uv ? 'out.uv = uv;' : ''} return out;
}
@fragment fn fs(input: VertexOutput) -> @location(0) vec4<f32> {
 ${uv && !diffuseOnly ? 'let sampled=textureSampleBias(texture0, sampler0, input.uv, matrices.lodBias); var color=input.color*sampled; if(matrices.textureEquation.x==1.0){color=vec4<f32>(matrices.textureFactor.rgb,matrices.textureFactor.a*sampled.a);} if(matrices.textureEquation.x==2.0){color=vec4<f32>(input.color.rgb,sampled.a);}' : 'let color=input.color;'}
 if(matrices.alphaEnable != 0.0){
  let a=clamp(color.a,0.0,1.0);let r=matrices.alphaRef;var alphaPassed=false;
  switch u32(matrices.alphaFunc){case 1u:{} case 2u:{alphaPassed=a<r;} case 3u:{alphaPassed=a==r;} case 4u:{alphaPassed=a<=r;} case 5u:{alphaPassed=a>r;} case 6u:{alphaPassed=a!=r;} case 7u:{alphaPassed=a>=r;} default:{alphaPassed=true;}}
  if(!alphaPassed){discard;}
 }
 ${logicalFormat === 25 && !blend ? 'return floor(clamp(color, vec4<f32>(0.0), vec4<f32>(1.0)) * vec4<f32>(31.0,31.0,31.0,1.0) + vec4<f32>(0.5)) / vec4<f32>(31.0,31.0,31.0,1.0);' : 'return color;'}
}`;
}
export class MoviesD3D9GPU {
  #logicalFormat = 22; #device; #context; #canvas; #color; #alternate; #resolvePipeline; #clearPipeline; #clearColorPipeline; #clearColorBinding; #clearUniform; #clearBinding; #resolveBindings = new Map(); #depth; #matrices; #pipelines = new PipelineCache(64,key=>{for(const r of this.#resources.values())if(r.bindGroups)for(const binding of r.bindGroups.keys())if(binding.startsWith(key+'/'))r.bindGroups.delete(binding);});
  #resources = new Map(); #backbuffer; #active; #nextResource = 1; #fatal = null; #disposed = false; #serial = 0; #depthInitialized = false; #colorInitialized = false;
  static async create({canvas, width = 256, height = 256, gpu = globalThis.navigator?.gpu} = {}) {
    if (!gpu || !canvas?.getContext || !canvas.transferToImageBitmap) throw new GraphicsUnsupportedError('Worker WebGPU and OffscreenCanvas required');
    checkedInteger(width, 1, 16384, 'width'); checkedInteger(height, 1, 16384, 'height');
    const adapter = await gpu.requestAdapter({powerPreference: 'high-performance'});
    if (!adapter) throw new GraphicsUnsupportedError('No WebGPU adapter');
    const device = await adapter.requestDevice();
    const backend = new MoviesD3D9GPU(); backend.#device = device; backend.#canvas = canvas;
    device.lost.then(info => { if (!backend.#disposed) backend.#fatal = new Error(`WebGPU device lost: ${info.reason}: ${info.message}`); });
    device.addEventListener('uncapturederror', event => { backend.#fatal = event.error; });
    try {
      checkedInteger(width, 1, device.limits.maxTextureDimension2D, 'device width');
      checkedInteger(height, 1, device.limits.maxTextureDimension2D, 'device height');
      canvas.width = width; canvas.height = height;
      const context = canvas.getContext('webgpu');
      if (!context) throw new GraphicsUnsupportedError('No worker WebGPU canvas context');
      backend.#context = context;
      const format = gpu.getPreferredCanvasFormat();
      if (!['bgra8unorm', 'rgba8unorm'].includes(format)) throw new GraphicsUnsupportedError(`Unsupported canvas format ${format}`);
      context.configure({device, format, usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT, alphaMode: 'opaque'});
      const target = label => device.createTexture({label, size:[width,height],format,usage:GPUTextureUsage.RENDER_ATTACHMENT|GPUTextureUsage.COPY_SRC|GPUTextureUsage.TEXTURE_BINDING});
      backend.#color=target('D3D9 current RGBA8 target');backend.#alternate=target('D3D9 RGB5/A1 draw-boundary resolve');
      const resolveModule=device.createShaderModule({code:`
@group(0) @binding(0) var source: texture_2d<f32>;
@vertex fn vs(@builtin(vertex_index) index:u32)->@builtin(position) vec4<f32>{
 let x=f32((index<<1u)&2u);let y=f32(index&2u);return vec4<f32>(x*2.0-1.0,1.0-y*2.0,0.0,1.0);
}
@fragment fn fs(@builtin(position) p:vec4<f32>)->@location(0) vec4<f32>{
 let color=textureLoad(source,vec2<i32>(p.xy),0);let levels=vec4<f32>(31.0,31.0,31.0,1.0);
 return floor(clamp(color,vec4<f32>(0.0),vec4<f32>(1.0))*levels+vec4<f32>(0.5))/levels;
}`});
      backend.#resolvePipeline=await device.createRenderPipelineAsync({layout:'auto',vertex:{module:resolveModule,entryPoint:'vs'},fragment:{module:resolveModule,entryPoint:'fs',targets:[{format}]},primitive:{topology:'triangle-list'}});
      for(const texture of [backend.#color,backend.#alternate])backend.#resolveBindings.set(texture,device.createBindGroup({layout:backend.#resolvePipeline.getBindGroupLayout(0),entries:[{binding:0,resource:texture.createView()}]}));
      device.pushErrorScope('validation');
      const depth = device.createTexture({label: 'D3D9 depth16 probe', size: [width, height], format: 'depth16unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT});
      const depthError = await device.popErrorScope();
      if (depthError) depth.destroy(); else backend.#depth = depth;
      backend.#backbuffer = backend.#active = {width, height, backbuffer: true};
      backend.#clearUniform=device.createBuffer({size:32,usage:GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST});
      const clearModule=device.createShaderModule({code:`
struct Clear {color:vec4<f32>,depth:f32};@group(0) @binding(0) var<uniform> value:Clear;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4<f32>{let x=f32((i<<1u)&2u);let y=f32(i&2u);return vec4<f32>(x*2.0-1.0,1.0-y*2.0,0.0,1.0);}
struct Result {@location(0) color:vec4<f32>,${backend.#depth?'@builtin(frag_depth) depth:f32,':''}};
@fragment fn fs()->Result {var result:Result;result.color=value.color;${backend.#depth?'result.depth=value.depth;':''}return result;}`});
      backend.#clearPipeline=await device.createRenderPipelineAsync({layout:'auto',vertex:{module:clearModule,entryPoint:'vs'},fragment:{module:clearModule,entryPoint:'fs',targets:[{format}]},primitive:{topology:'triangle-list'},...(backend.#depth?{depthStencil:{format:'depth16unorm',depthWriteEnabled:true,depthCompare:'always'}}:{})});
      backend.#clearBinding=device.createBindGroup({layout:backend.#clearPipeline.getBindGroupLayout(0),entries:[{binding:0,resource:{buffer:backend.#clearUniform}}]});
      const clearColorModule=device.createShaderModule({code:`
struct Clear {color:vec4<f32>,depth:f32};@group(0) @binding(0) var<uniform> value:Clear;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4<f32>{let x=f32((i<<1u)&2u);let y=f32(i&2u);return vec4<f32>(x*2.0-1.0,1.0-y*2.0,0.0,1.0);}
@fragment fn fs()->@location(0) vec4<f32>{return value.color;}`});
      backend.#clearColorPipeline=await device.createRenderPipelineAsync({layout:'auto',vertex:{module:clearColorModule,entryPoint:'vs'},fragment:{module:clearColorModule,entryPoint:'fs',targets:[{format}]},primitive:{topology:'triangle-list'}});
      backend.#clearColorBinding=device.createBindGroup({layout:backend.#clearColorPipeline.getBindGroupLayout(0),entries:[{binding:0,resource:{buffer:backend.#clearUniform}}]});
      backend.#matrices = device.createBuffer({size: 256, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST});
      const info = adapter.info;
      backend.capabilities = Object.freeze({schemaVersion: 1, backend: 'WebGPU', width, height, colorFormat: format,
        depthFormat: backend.#depth ? 'depth16unorm' : null,
        adapter: info ? {vendor: info.vendor, architecture: info.architecture, device: info.device, description: info.description, isFallbackAdapter: info.isFallbackAdapter ?? adapter.isFallbackAdapter ?? null} : null,
        limits: {maxTextureDimension2D: device.limits.maxTextureDimension2D, maxBufferSize: device.limits.maxBufferSize, maxVertexBufferArrayStride: device.limits.maxVertexBufferArrayStride},
        renderTargetFormats: [22,25],
        supported: ['A1R5G5B5-output-quantization', 'classic-blend-factors1-through11-ADD', 'SRCALPHA-INVSRCALPHA-ADD', 'SRCCOLOR-INVSRCCOLOR-ADD-diffuse-only', 'ZERO-SRCCOLOR-ADD-diffuseRGB-textureAlpha', 'RGBA8-blend-RGB5A1-resolve-per-draw', 'alpha-test-all-comparisons', 'XYZ_DIFFUSE', 'XYZ_DIFFUSE_UV', 'triangle-list', 'uint16-indices', 'uint32-indices', 'world-view-projection', 'RGBA8-texture-point-linear-wrap-mirror-clamp-single-level', 'clear', 'RGBA8-readback', 'ImageBitmap-present'],
        unsupported: ['lighting', 'fog', 'clip-planes', 'vertex-blending', 'programmable-shaders', 'multitexture', 'blend-factors12-and-above', 'non-ADD-blend-ops', 'separate-alpha', 'stencil', 'MSAA', 'mipmaps', 'compressed-textures', 'resize-reset']});
      await backend.validate();
      return backend;
    } catch (error) { backend.dispose(); throw error; }
  }
  async #pipeline(key,vertexFormat,logicalFormat,depthState,blend,diffuseOnly){
    return await this.#pipelines.get(key,async()=>{
        const device=this.#device,format=this.capabilities.colorFormat,useDepth=!!depthState;
        const rhw=vertexFormat==='XYZRHW_DIFFUSE_UV',uv=vertexFormat!=='XYZ_DIFFUSE';
        const module = device.createShaderModule({code: shader(uv, logicalFormat, rhw, !!blend, diffuseOnly), label: 'Bounded XYZ diffuse fixed function'});
        const compilation=await module.getCompilationInfo();
        const errors=compilation.messages.filter(message=>message.type==='error');
        if(errors.length)throw new Error('WGSL compilation: '+errors.map(message=>`${message.lineNum}:${message.linePos} ${message.message}`).join('; '));
        const descriptor = {label: `XYZ${uv ? ' UV' : ''}${useDepth ? ' depth16' : ''}`, layout: 'auto',
          vertex: {module, entryPoint: 'vs', buffers: [{arrayStride: rhw ? 28 : uv ? 24 : 16, attributes: [
            {shaderLocation: 0, offset: 0, format: rhw ? 'float32x4' : 'float32x3'}, {shaderLocation: 1, offset: rhw ? 16 : 12, format: 'uint32'},
            ...(uv ? [{shaderLocation: 2, offset: rhw ? 20 : 16, format: 'float32x2'}] : [])]}]},
          fragment: {module, entryPoint:'fs',targets:[{format,...(blend?{blend:{color:blend.color,alpha:blend.alpha}}:{})}]},
          primitive: {topology: 'triangle-list', cullMode: 'none'},
          ...(useDepth ? {depthStencil: {format: 'depth16unorm', depthWriteEnabled: depthState.write, depthCompare: depthState.compare}} : {})};
        const pipeline = await device.createRenderPipelineAsync(descriptor);
      this.#check();
      return {pipeline,uniform:this.#device.createBindGroup({layout:pipeline.getBindGroupLayout(0),entries:[{binding:0,resource:{buffer:this.#matrices}}]})};
    });
  }
  #check() { if (this.#disposed) throw new Error('Graphics backend disposed'); if (this.#fatal) throw this.#fatal; }
  #resource(handle, type) { this.#check(); const r = this.#resources.get(handle); if (!r || r.type !== type) throw new TypeError(`Invalid ${type} handle`); return r; }
  #store(type, value) { const handle = Object.freeze({id: this.#nextResource++, type}); this.#resources.set(handle, {type, ...value}); return handle; }
  #buffer(bytes, usage) {
    checkedInteger(bytes.byteLength, 1, this.capabilities.limits.maxBufferSize, 'buffer size');
    const size = Math.ceil(bytes.byteLength / 4) * 4;
    const buffer = this.#device.createBuffer({size, usage: usage | GPUBufferUsage.COPY_DST});
    const padded = new Uint8Array(size); padded.set(bytes); this.#device.queue.writeBuffer(buffer, 0, padded); return buffer;
  }
  uploadVertices(input, format = 'XYZ_DIFFUSE') {
    this.#check(); const data = validateVertexBytes(input, format);
    return this.#store('vertices', {...data, buffer: this.#buffer(data.bytes, GPUBufferUsage.VERTEX)});
  }
  uploadIndices(input, format = 'uint16') {
    this.#check(); const data = validateIndexBytes(input, format);
    return this.#store('indices', {...data, buffer: this.#buffer(data.bytes, GPUBufferUsage.INDEX)});
  }
  uploadTextureRGBA(width, height, input) {
    this.#check(); const max = this.capabilities.limits.maxTextureDimension2D;
    checkedInteger(width, 1, max, 'texture width'); checkedInteger(height, 1, max, 'texture height');
    const bytes = bytesOf(input); if (bytes.length !== width * height * 4) throw new RangeError('RGBA texture byte count mismatch');
    const texture = this.#device.createTexture({size: [width, height], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST});
    this.#device.queue.writeTexture({texture}, bytes, {bytesPerRow: width * 4, rowsPerImage: height}, [width, height]);
    return this.#store('texture', {texture, bindGroups: new Map()});
  }
  /* D3D9 render-target textures (D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT): a GPU
   * color target in the presentation format that is also sampleable. Depth is
   * per-target because WebGPU attachments must match in size, whereas D3D9 shares
   * one larger depth surface; contents of the shared depth across targets are not
   * modelled. Initial color is zero (D3D9 leaves it undefined); depth must be
   * cleared before depth-tested draws. */
  createRenderTarget(width, height) {
    this.#check(); const max = this.capabilities.limits.maxTextureDimension2D;
    checkedInteger(width, 1, max, 'render target width'); checkedInteger(height, 1, max, 'render target height');
    const texture = this.#device.createTexture({label: `D3D9 render target ${width}x${height}`, size: [width, height], format: this.capabilities.colorFormat,
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST});
    const depth = this.#depthFor(this.#backbuffer) ? this.#device.createTexture({label: `D3D9 render target depth ${width}x${height}`, size: [width, height], format: 'depth16unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT}) : null;
    const target = {width, height, color: texture, alternate: null, depth, colorInitialized: true, depthInitialized: false, logicalFormat: 22};
    return this.#store('texture', {texture, bindGroups: new Map(), target});
  }
  #renderTarget(handle) { const r = this.#resource(handle, 'texture'); if (!r.target) throw new GraphicsUnsupportedError('Texture is not a render target'); return r; }
  /* GetRenderTargetData: whole render-target texture as RGBA8 rows. */
  async readRenderTarget(handle) {
    const r = this.#renderTarget(handle), {width, height} = r.target, stride = Math.ceil(width * 4 / 256) * 256;
    const buffer = this.#device.createBuffer({size: stride * height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ});
    try {
      const encoder = this.#device.createCommandEncoder();
      encoder.copyTextureToBuffer({texture: r.texture}, {buffer, bytesPerRow: stride, rowsPerImage: height}, [width, height]);
      this.#device.queue.submit([encoder.finish()]); await buffer.mapAsync(GPUMapMode.READ); this.#check();
      const mapped = new Uint8Array(buffer.getMappedRange()), rgba = new Uint8Array(width * height * 4);
      for (let y = 0; y < height; y++) rgba.set(mapped.subarray(y * stride, y * stride + width * 4), y * width * 4);
      if (this.capabilities.colorFormat === 'bgra8unorm') for (let i = 0; i < rgba.length; i += 4) { const red = rgba[i + 2]; rgba[i + 2] = rgba[i]; rgba[i] = red; }
      buffer.unmap(); return {width, height, rgba};
    } finally { buffer.destroy(); }
  }
  /* UpdateSurface (SYSTEMMEM -> render target): whole-texture RGBA8 replacement. */
  writeRenderTarget(handle, width, height, input) {
    const r = this.#renderTarget(handle);
    if (width !== r.target.width || height !== r.target.height) throw new RangeError('Render target write must cover the whole texture');
    const source = bytesOf(input); if (source.length !== width * height * 4) throw new RangeError('Render target byte count mismatch');
    let bytes = source;
    if (this.capabilities.colorFormat === 'bgra8unorm') { bytes = new Uint8Array(source); for (let i = 0; i < bytes.length; i += 4) { const red = bytes[i]; bytes[i] = bytes[i + 2]; bytes[i + 2] = red; } }
    this.#device.queue.writeTexture({texture: r.texture}, bytes, {bytesPerRow: width * 4, rowsPerImage: height}, [width, height]);
    r.target.colorInitialized = true; if (r.target === this.#active) this.#colorInitialized = true;
  }
  #depthFor(record) { return record === this.#active ? this.#depth : record.depth; }
  #saveTarget() { const t = this.#active; t.color = this.#color; t.alternate = this.#alternate; t.depth = this.#depth; t.colorInitialized = this.#colorInitialized; t.depthInitialized = this.#depthInitialized; t.logicalFormat = this.#logicalFormat; }
  #loadTarget(t) { this.#active = t; this.#color = t.color; this.#alternate = t.alternate; this.#depth = t.depth; this.#colorInitialized = t.colorInitialized; this.#depthInitialized = t.depthInitialized; this.#logicalFormat = t.logicalFormat; }
  setRenderTarget(handle = null) {
    this.#check();
    const target = handle === null ? this.#backbuffer : this.#resource(handle, 'texture').target;
    if (!target) throw new GraphicsUnsupportedError('Texture is not a render target');
    if (target !== this.#active) { this.#saveTarget(); this.#loadTarget(target); }
    return {width: target.width, height: target.height};
  }
  #backbufferState() { if (this.#active !== this.#backbuffer) return this.#backbuffer; this.#saveTarget(); return this.#backbuffer; }
  setRenderTargetFormat(format) {
    this.#check();
    if (![22, 25].includes(format)) throw new GraphicsUnsupportedError(`Unsupported logical target format ${format}`);
    if (this.#active !== this.#backbuffer) throw new Error('Backbuffer format changes require the backbuffer target');
    this.#logicalFormat = format; this.#colorInitialized = false; this.#depthInitialized = false; return format;
  }
  clear({color = 0xff000000, depth = 1, scissor = null, zbuffer = true} = {}) {
    this.#check(); const source = rgbaFromD3DColor(color), rgba = this.#logicalFormat === 25 ? quantizeA1R5G5B5(source) : source;
    if (!Number.isFinite(depth) || depth < 0 || depth > 1) throw new RangeError('Invalid clear depth');
    if (typeof zbuffer !== 'boolean') throw new TypeError('Clear zbuffer flag must be boolean');
    const clip=validateScissor(scissor,this.#active.width,this.#active.height);
    // D3DCLEAR_TARGET alone leaves depth untouched (no depth attachment in the pass).
    if(!zbuffer||!this.#depth){
      const values=new Float32Array(8);values.set(rgba);this.#device.queue.writeBuffer(this.#clearUniform,0,values);
      const encoder=this.#device.createCommandEncoder();
      const full=!clip||(clip.x===0&&clip.y===0&&clip.width===this.#active.width&&clip.height===this.#active.height);
      const pass=encoder.beginRenderPass({colorAttachments:[{view:this.#color.createView(),loadOp:full?'clear':(this.#colorInitialized?'load':'clear'),clearValue:full?rgba:[0,0,0,0],storeOp:'store'}]});
      if(!full){pass.setPipeline(this.#clearColorPipeline);pass.setBindGroup(0,this.#clearColorBinding);pass.setScissorRect(clip.x,clip.y,clip.width,clip.height);pass.draw(3);}
      pass.end();this.#device.queue.submit([encoder.finish()]);this.#colorInitialized=true;return;
    }
    if(clip){
      const values=new Float32Array(8);values.set(rgba);values[4]=depth;this.#device.queue.writeBuffer(this.#clearUniform,0,values);
      const encoder=this.#device.createCommandEncoder(),pass=encoder.beginRenderPass({colorAttachments:[{view:this.#color.createView(),loadOp:this.#colorInitialized?'load':'clear',clearValue:[0,0,0,0],storeOp:'store'}],
        ...(this.#depth?{depthStencilAttachment:{view:this.#depth.createView(),depthLoadOp:this.#depthInitialized?'load':'clear',depthClearValue:0,depthStoreOp:'store'}}:{})});
      pass.setPipeline(this.#clearPipeline);pass.setBindGroup(0,this.#clearBinding);pass.setScissorRect(clip.x,clip.y,clip.width,clip.height);pass.draw(3);pass.end();this.#device.queue.submit([encoder.finish()]);
      this.#colorInitialized=true;this.#depthInitialized=!!this.#depth;return;
    }
    const encoder = this.#device.createCommandEncoder();
    const pass = encoder.beginRenderPass({colorAttachments: [{view: this.#color.createView(), clearValue: rgba, loadOp: 'clear', storeOp: 'store'}],
      ...(this.#depth ? {depthStencilAttachment: {view: this.#depth.createView(), depthClearValue: depth, depthLoadOp: 'clear', depthStoreOp: 'store'}} : {})});
    pass.end(); this.#device.queue.submit([encoder.finish()]); this.#colorInitialized = true; this.#depthInitialized = !!this.#depth;
  }
  async drawIndexed({vertices, indices, firstIndex = 0, indexCount, baseVertex = 0, world, view, projection, texture = null, sampler = undefined, alphaTest = undefined, depthTest = false, depthCompare = 'less-equal', depthWrite = true, blend = false, blendMode = 'source-alpha', blendFactors = undefined, textureEquation = 'modulate', textureFactor = 0xffffffff, scissor = null, viewport = null, ...unknown}) {
    this.#check(); if (Object.keys(unknown).length) throw new GraphicsUnsupportedError(`Unknown draw features: ${Object.keys(unknown).join(', ')}`);
    const vb = this.#resource(vertices, 'vertices'), ib = this.#resource(indices, 'indices');
    if(typeof blend!=='boolean')throw new TypeError('Blend enable must be boolean');
    if (typeof depthTest !== 'boolean' || typeof depthWrite !== 'boolean') throw new TypeError('Depth enable/write must be boolean');
    if(!['less-equal','always'].includes(depthCompare))throw new GraphicsUnsupportedError('Unsupported depth comparison');
    if (depthTest && !this.#depth) throw new GraphicsUnsupportedError('Depth16 unavailable');
    if (!this.#colorInitialized || (depthTest && !this.#depthInitialized)) throw new Error('Clear render targets before drawing');
    const rhw=vb.format==='XYZRHW_DIFFUSE_UV',uv=vb.format!=='XYZ_DIFFUSE';
    if (textureEquation!=='diffuse' && uv !== !!texture) throw new GraphicsUnsupportedError('UV format requires exactly one texture; untextured format forbids textures');
    const tex = texture ? this.#resource(texture, 'texture') : null;
    if (tex?.target && tex.target === this.#active) throw new GraphicsUnsupportedError('Sampling the active render target is undefined in D3D9');
    if(!['modulate','factor-rgb-texture-alpha','diffuse','diffuse-rgb-texture-alpha'].includes(textureEquation)||(textureEquation==='factor-rgb-texture-alpha'&&!tex))throw new GraphicsUnsupportedError('Unsupported texture equation');
    const blendState=resolveBlend({blend,blendMode,blendFactors});
    if(textureEquation==='diffuse'&&tex)throw new GraphicsUnsupportedError('Diffuse equation requires no texture');
    if(textureEquation==='diffuse-rgb-texture-alpha'&&!tex)throw new GraphicsUnsupportedError('Diffuse RGB texture alpha requires a texture');
    checkedInteger(textureFactor,0,0xffffffff,'texture factor');
    indexCount ??= ib.count - firstIndex; validateIndexedRange(vb, ib, firstIndex, indexCount, baseVertex);
    const sampling=samplerDescriptor(sampler);
    const matrices = new Float32Array(64); matrices[48]=sampling.state.lodBias; const alpha=validateAlphaTest(alphaTest);matrices[49]=alpha.enabled?1:0;matrices[50]=alpha.func;matrices[51]=alpha.reference/255; matrices.set(matrixBytes(world)); matrices.set(matrixBytes(view), 16); matrices.set(matrixBytes(projection), 32);
    const {width, height} = this.#active;
    const vp = viewport ?? {x: 0, y: 0, width, height, minDepth: 0, maxDepth: 1};
    const {x, y, width: w, height: h, minDepth = 0, maxDepth = 1} = vp;
    if (![x, y, w, h, minDepth, maxDepth].every(Number.isFinite) || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > width || y + h > height || minDepth < 0 || maxDepth > 1 || minDepth > maxDepth) throw new RangeError('Invalid viewport');
    if(rhw){
      const data=new DataView(vb.bytes.buffer,vb.bytes.byteOffset,vb.bytes.byteLength);
      for(let i=0;i<vb.count;i++){
        const at=i*28,reciprocal=data.getFloat32(at+12,true);
        const clip=[(2*(data.getFloat32(at,true)-x+.5)/w-1)/reciprocal,(1-2*(data.getFloat32(at+4,true)-y+.5)/h)/reciprocal,data.getFloat32(at+8,true)/reciprocal,1/reciprocal];
        if(clip.some(v=>!Number.isFinite(Math.fround(v))))throw new RangeError('Transformed clip position overflows float32');
      }
    }
    const clip=validateScissor(scissor,width,height);
    matrices.set([x,y,w,h],52);
    matrices.set([(textureFactor>>>16&255)/255,(textureFactor>>>8&255)/255,(textureFactor&255)/255,(textureFactor>>>24)/255],56);matrices[60]=textureEquation==='diffuse-rgb-texture-alpha'?2:textureEquation==='factor-rgb-texture-alpha'?1:0;
    const key = `${this.#logicalFormat}/${blendState?blendState.source+','+blendState.destination:'off'}/${vb.format}/${textureEquation==='diffuse'}/${depthTest ? depthCompare+'/'+depthWrite : 'off'}`;
    const {pipeline,uniform}=await this.#pipeline(key,vb.format,this.#logicalFormat,depthTest?{compare:depthCompare,write:depthWrite}:null,blendState,textureEquation==='diffuse');
    this.#device.queue.writeBuffer(this.#matrices, 0, matrices);
    const encoder = this.#device.createCommandEncoder();
    const pass = encoder.beginRenderPass({colorAttachments: [{view: this.#color.createView(), loadOp: 'load', storeOp: 'store'}],
      ...(depthTest ? {depthStencilAttachment: {view: this.#depth.createView(), depthLoadOp: 'load', depthStoreOp: 'store'}} : {})});
    pass.setPipeline(pipeline); pass.setBindGroup(0, uniform);
    if (tex) {
      const bindingKey=key+'/'+JSON.stringify(sampling.descriptor);
      if (!tex.bindGroups.has(bindingKey)) tex.bindGroups.set(bindingKey, this.#device.createBindGroup({layout: pipeline.getBindGroupLayout(1), entries: [{binding: 0, resource: tex.texture.createView()}, {binding: 1, resource: this.#device.createSampler(sampling.descriptor)}]}));
      pass.setBindGroup(1, tex.bindGroups.get(bindingKey));
    }
    pass.setViewport(x, y, w, h, rhw ? 0 : minDepth, rhw ? 1 : maxDepth);
    if(clip)pass.setScissorRect(clip.x,clip.y,clip.width,clip.height);
    pass.setVertexBuffer(0, vb.buffer); pass.setIndexBuffer(ib.buffer, ib.format);
    pass.drawIndexed(indexCount, 1, firstIndex, baseVertex, 0); pass.end();
    // Compatibility model: intra-draw blends accumulate in RGBA8. Canonical
    // RGB5/A1 UNORM8 feedback is published at EVERY guest draw boundary.
    if(this.#logicalFormat===25){
      const resolve=encoder.beginRenderPass({colorAttachments:[{view:this.#alternate.createView(),loadOp:'clear',clearValue:[0,0,0,0],storeOp:'store'}]});
      resolve.setPipeline(this.#resolvePipeline);resolve.setBindGroup(0,this.#resolveBindings.get(this.#color));resolve.draw(3);resolve.end();
    }
    this.#device.queue.submit([encoder.finish()]);
    if(this.#logicalFormat===25)[this.#color,this.#alternate]=[this.#alternate,this.#color];
  }
  async readPixels() {
    this.#check(); const bb = this.#backbufferState(); if (!bb.colorInitialized) throw new Error('Readback before clear');
    const {width, height, colorFormat} = this.capabilities, stride = Math.ceil(width * 4 / 256) * 256;
    const buffer = this.#device.createBuffer({size: stride * height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ});
    try {
      const encoder = this.#device.createCommandEncoder();
      encoder.copyTextureToBuffer({texture: bb.color}, {buffer, bytesPerRow: stride, rowsPerImage: height}, [width, height]);
      this.#device.queue.submit([encoder.finish()]); await buffer.mapAsync(GPUMapMode.READ); this.#check();
      const mapped = new Uint8Array(buffer.getMappedRange()), rgba = new Uint8Array(width * height * 4);
      for (let y = 0; y < height; y++) rgba.set(mapped.subarray(y * stride, y * stride + width * 4), y * width * 4);
      if (colorFormat === 'bgra8unorm') for (let i = 0; i < rgba.length; i += 4) { const red = rgba[i + 2]; rgba[i + 2] = rgba[i]; rgba[i] = red; }
      buffer.unmap(); return {width, height, rgba};
    } finally { buffer.destroy(); }
  }
  present(onBitmap) {
    this.#check(); const bb = this.#backbufferState(); if (!bb.colorInitialized) throw new Error('Present before clear');
    if (typeof onBitmap !== 'function') throw new TypeError('Present requires an explicit ImageBitmap consumer');
    const encoder = this.#device.createCommandEncoder();
    encoder.copyTextureToTexture({texture: bb.color}, {texture: this.#context.getCurrentTexture()}, [this.capabilities.width, this.capabilities.height]);
    this.#device.queue.submit([encoder.finish()]);
    const bitmap = this.#canvas.transferToImageBitmap(), serial = ++this.#serial;
    try { return onBitmap(bitmap, serial); } catch (error) { bitmap.close(); throw error; }
  }
  async validate() { this.#check(); await this.#device.queue.onSubmittedWorkDone(); await new Promise(resolve => setTimeout(resolve, 0)); this.#check(); }
  release(handle) { this.#check(); const r = this.#resources.get(handle); if (!r) throw new TypeError('Invalid resource handle'); if (r.target && r.target === this.#active) throw new Error('Cannot release the active render target'); r.buffer?.destroy(); r.texture?.destroy(); r.target?.depth?.destroy(); this.#resources.delete(handle); }
  dispose() {
    if (this.#disposed) return; this.#disposed = true;
    if (this.#backbuffer && this.#active !== this.#backbuffer) { this.#saveTarget(); this.#loadTarget(this.#backbuffer); }
    for (const resource of this.#resources.values()) { resource.buffer?.destroy(); resource.texture?.destroy(); resource.target?.depth?.destroy(); }
    this.#pipelines.clear(); this.#resources.clear(); this.#matrices?.destroy(); this.#clearUniform?.destroy(); this.#color?.destroy(); this.#alternate?.destroy(); this.#resolveBindings.clear(); this.#depth?.destroy(); this.#context?.unconfigure(); this.#device?.destroy();
  }
}
