import test from 'node:test';
import assert from 'node:assert/strict';
import {validateCommand} from '../public/graphics-protocol.js';
test('depth compare/write states are independently typed; all eight D3DCMP comparisons map to WebGPU',()=>{
 for(const depthCompare of ['never','less','equal','less-equal','greater','not-equal','greater-equal','always'])for(const depthWrite of [false,true]) {
  assert.equal(validateCommand('drawIndexed',{vertices:1,indices:2,depthTest:true,depthCompare,depthWrite}),0);
 }
 for(const depthCompare of ['lequal','',8,null])assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,depthCompare}),/depth comparison/);
 for(const depthWrite of [0,1,'false',null])assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,depthWrite}),/depth write/);
});
test('blend command exposes only audited source-alpha mode enable, no arbitrary factors',()=>{
 for(const blend of [false,true])assert.equal(validateCommand('drawIndexed',{vertices:1,indices:2,blend}),0);
 for(const blend of [1,'src-over',{},null])assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,blend}),/blend/);
 assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,blend:true,srcFactor:'one'}),/fields/);
});
test('scissor wire validation covers draws and clears, with explicit empty rectangles',()=>{
 const scissor={x:10,y:20,width:30,height:40};
 for(const command of ['drawIndexed','clear']){
  const args=command==='drawIndexed'?{vertices:1,indices:2}:{};
  assert.equal(validateCommand(command,{...args,scissor}),0);assert.equal(validateCommand(command,{...args,scissor:{...scissor,width:0}}),0);
  for(const bad of [{...scissor,x:-1},{...scissor,height:1.5},{...scissor,width:5000},{...scissor,enabled:true}])assert.throws(()=>validateCommand(command,{...args,scissor:bad}));
 }
});

test('texture equation defaults preserve old draw shape and only audited factor mode is accepted',()=>{
 const old={vertices:1,indices:2,texture:3};assert.equal(validateCommand('drawIndexed',old),0);
 for(const textureFactor of [0,0x80402010,0xffffffff])assert.equal(validateCommand('drawIndexed',{...old,textureEquation:'factor-rgb-texture-alpha',textureFactor}),0);
 for(const textureEquation of ['selectarg2',1,null])assert.throws(()=>validateCommand('drawIndexed',{...old,textureEquation}),/equation/);
 for(const textureFactor of [-1,0x100000000,1.5,null])assert.throws(()=>validateCommand('drawIndexed',{...old,textureFactor}),/factor/);
 assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,textureEquation:'factor-rgb-texture-alpha'}),/requires texture/);
});

test('classic ADD factors are orthogonal to each implemented source equation',()=>{
 for(const textureEquation of ['modulate','factor-rgb-texture-alpha','diffuse','diffuse-rgb-texture-alpha'])for(let source=1;source<=11;source++)for(let destination=1;destination<=11;destination++){
  assert.equal(validateCommand('drawIndexed',{vertices:1,indices:2,texture:textureEquation==='diffuse'?null:3,blend:true,blendFactors:{source,destination},textureEquation}),0);
 }
 for(const blendFactors of [{source:0,destination:1},{source:12,destination:1},{source:1,destination:12},{source:1.5,destination:2},{source:1,destination:1,extra:1}])assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,blendFactors}));
 assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,texture:3,textureEquation:'diffuse'}));
 assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,textureEquation:'diffuse-rgb-texture-alpha'}));
});
test('render-target commands validate dimensions and nullable texture handles',()=>{
  validateCommand('createRenderTarget',{width:512,height:512});
  for(const bad of [{width:0,height:1},{width:1,height:5000},{width:1,height:1,format:21}])assert.throws(()=>validateCommand('createRenderTarget',bad));
  validateCommand('setRenderTarget',{texture:null});validateCommand('setRenderTarget',{texture:7});
  for(const bad of [{},{texture:0},{texture:'7'},{texture:1,extra:1}])assert.throws(()=>validateCommand('setRenderTarget',bad));
});
test('render-target copies require two distinct typed handles',()=>{
  validateCommand('copyRenderTarget',{source:1,destination:2});
  for(const bad of [{source:1,destination:1},{source:1},{source:0,destination:2},{source:1,destination:2,filter:2}])assert.throws(()=>validateCommand('copyRenderTarget',bad));
});
test('depth bias, colour write mask and blend constant are validated',()=>{
 assert.equal(validateCommand('drawIndexed',{vertices:1,indices:2,depthBias:0.001,slopeScaleDepthBias:-1,colorWriteMask:7,blendConstant:0x80ff0000}),0);
 assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,depthBias:NaN}),/depthBias/);
 assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,colorWriteMask:16}),/colour write mask/);
 assert.throws(()=>validateCommand('drawIndexed',{vertices:1,indices:2,blendFactors:{source:5,destination:6,operation:6}}),/blend operation/);
 assert.equal(validateCommand('drawIndexed',{vertices:1,indices:2,blend:true,blendFactors:{source:14,destination:15,operation:3,alphaSource:2,alphaDestination:1,alphaOperation:5}}),0);
});
