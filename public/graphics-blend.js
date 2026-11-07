/* D3DBLEND1..11 mapped independently for RGB and alpha.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dblend
 * https://www.w3.org/TR/webgpu/#enumdef-gpublendfactor */
const rgb=[null,'zero','one','src','one-minus-src','src-alpha','one-minus-src-alpha','dst-alpha','one-minus-dst-alpha','dst','one-minus-dst','src-alpha-saturated'];
const alpha=[null,'zero','one','src-alpha','one-minus-src-alpha','src-alpha','one-minus-src-alpha','dst-alpha','one-minus-dst-alpha','dst-alpha','one-minus-dst-alpha','one'];
export function resolveBlend({blend=false,blendMode='source-alpha',blendFactors}={}) {
 if(typeof blend!=='boolean')throw new TypeError('Invalid blend enable');
 const legacy={'source-alpha':[5,6],'source-color':[3,4],'destination-times-source':[1,3]};
 if(!Object.hasOwn(legacy,blendMode))throw new TypeError('Unsupported blend mode');
 let pair=legacy[blendMode];
 if(blendFactors!==undefined){
  if(!blendFactors||typeof blendFactors!=='object'||Array.isArray(blendFactors)||Object.keys(blendFactors).length!==2||!Object.hasOwn(blendFactors,'source')||!Object.hasOwn(blendFactors,'destination'))throw new TypeError('Invalid blend factors');
  pair=[blendFactors.source,blendFactors.destination];
  if(pair.some(v=>!Number.isInteger(v)||v<1||v>11))throw new RangeError('Only classic blend factors1..11 implemented');
  if(blendMode!=='source-alpha'&&(pair[0]!==legacy[blendMode][0]||pair[1]!==legacy[blendMode][1]))throw new TypeError('Conflicting legacy blend mode and factors');
 }
 if(!blend)return null;
 const [source,destination]=pair;
 return {source,destination,color:{operation:'add',srcFactor:rgb[source],dstFactor:rgb[destination]},alpha:{operation:'add',srcFactor:alpha[source],dstFactor:alpha[destination]}};
}
/* Serialized graphics commands await compilation. Failed creation is never
 * cached or acknowledged; evictions discard dependent texture bind groups. */
export class PipelineCache {
 #entries=new Map();
 constructor(limit=64,onEvict=()=>{}){if(!Number.isInteger(limit)||limit<1||typeof onEvict!=='function')throw new TypeError('Invalid pipeline cache');this.limit=limit;this.onEvict=onEvict;}
 get size(){return this.#entries.size;}
 async get(key,create){
  if(this.#entries.has(key)){const value=this.#entries.get(key);this.#entries.delete(key);this.#entries.set(key,value);return value;}
  const value=await create();
  if(this.#entries.size>=this.limit){const oldest=this.#entries.keys().next().value;this.#entries.delete(oldest);this.onEvict(oldest);}
  this.#entries.set(key,value);return value;
 }
 clear(){for(const key of this.#entries.keys())this.onEvict(key);this.#entries.clear();}
}
