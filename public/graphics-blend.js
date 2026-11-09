/* D3DBLEND factors mapped independently for RGB and alpha; D3DBLENDOP 1..5.
 * BOTHSRCALPHA/BOTHINVSRCALPHA (12/13) are rewritten by the C layer into explicit
 * pairs; BLENDFACTOR/INVBLENDFACTOR (14/15) use the WebGPU blend constant.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dblend
 * https://www.w3.org/TR/webgpu/#enumdef-gpublendfactor */
const rgb=[null,'zero','one','src','one-minus-src','src-alpha','one-minus-src-alpha','dst-alpha','one-minus-dst-alpha','dst','one-minus-dst','src-alpha-saturated',null,null,'constant','one-minus-constant'];
const alpha=[null,'zero','one','src-alpha','one-minus-src-alpha','src-alpha','one-minus-src-alpha','dst-alpha','one-minus-dst-alpha','dst-alpha','one-minus-dst-alpha','one',null,null,'constant','one-minus-constant'];
const ops=[null,'add','subtract','reverse-subtract','min','max'];
const FIELDS=['source','destination','operation','alphaSource','alphaDestination','alphaOperation'];
export function resolveBlend({blend=false,blendMode='source-alpha',blendFactors}={}) {
 if(typeof blend!=='boolean')throw new TypeError('Invalid blend enable');
 const legacy={'source-alpha':[5,6],'source-color':[3,4],'destination-times-source':[1,3]};
 if(!Object.hasOwn(legacy,blendMode))throw new TypeError('Unsupported blend mode');
 let pair=legacy[blendMode],operation=1,alphaPair=null,alphaOperation=null;
 if(blendFactors!==undefined){
  if(!blendFactors||typeof blendFactors!=='object'||Array.isArray(blendFactors)||!Object.hasOwn(blendFactors,'source')||!Object.hasOwn(blendFactors,'destination')||Object.keys(blendFactors).some(k=>!FIELDS.includes(k)))throw new TypeError('Invalid blend factors');
  pair=[blendFactors.source,blendFactors.destination];
  const factor=v=>Number.isInteger(v)&&v>=1&&v<=15&&v!==12&&v!==13;
  if(!pair.every(factor))throw new RangeError('Unsupported blend factor');
  if(blendFactors.operation!==undefined){operation=blendFactors.operation;if(!ops[operation])throw new RangeError('Unsupported blend operation');}
  if(blendFactors.alphaSource!==undefined||blendFactors.alphaDestination!==undefined||blendFactors.alphaOperation!==undefined){
   alphaPair=[blendFactors.alphaSource,blendFactors.alphaDestination];alphaOperation=blendFactors.alphaOperation??1;
   if(!alphaPair.every(factor)||!ops[alphaOperation])throw new RangeError('Unsupported separate alpha blend');
  }
  if(blendMode!=='source-alpha'&&(pair[0]!==legacy[blendMode][0]||pair[1]!==legacy[blendMode][1]))throw new TypeError('Conflicting legacy blend mode and factors');
 }
 if(!blend)return null;
 const [source,destination]=pair,[aSource,aDestination]=alphaPair??pair,aOperation=alphaOperation??operation;
 // WebGPU requires factor 'one' for min/max; D3D9 ignores the factors for those ops.
 const component=(op,src,dst,table)=>op>=4?{operation:ops[op],srcFactor:'one',dstFactor:'one'}:{operation:ops[op],srcFactor:table[src],dstFactor:table[dst]};
 const usesConstant=[source,destination,aSource,aDestination].some(f=>f===14||f===15);
 return {source,destination,operation,alphaSource:aSource,alphaDestination:aDestination,alphaOperation:aOperation,usesConstant,
  color:component(operation,source,destination,rgb),alpha:component(aOperation,aSource,aDestination,alpha)};
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
