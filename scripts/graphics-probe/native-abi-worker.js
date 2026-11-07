import {MoviesGraphicsClient} from './graphics-client.js';
import {createMoviesD3D9Host} from './d3d9-host.js';
import createNativeD3D9Probe from './native-abi-module.js';
self.onmessage = async ({data}) => {
  if (data.type !== 'run') return;
  let client, host;
  try {
    client = new MoviesGraphicsClient({width: 190, height: 128, presentationAck: data.ack, presentationPort: data.presentationPort, commandPort: data.commandPort, responseBuffer: data.responseBuffer});
    host = createMoviesD3D9Host({client, getWindow: hwnd => hwnd === 42 ? {clientWidth: 190, clientHeight: 128} : null, presentationIntervalMask: 1});
    const targetFormat=data.targetFormat===25?25:22;
    const checks = [], commands = {}, logs = [], blendComparisons=[],lastUploads={};
    const quantize = rgba => rgba.map((v,i)=>{const scale=i===3?1:31;return Math.round(Math.floor(v/255*scale+.5)/scale*255);});
    const blend = (dst,src) => src.map((v,i)=>Math.round(v*src[3]/255+dst[i]*(1-src[3]/255)));
    let canonicalBlend=null,factorDestination=null,sourceColorDestination=null,multiplyDestination=null,additiveDestination=null;

    const module = await createNativeD3D9Probe({noInitialRun: true,
      print: line => { if (logs.length < 20) logs.push(line); }, printErr: line => { if (logs.length < 20) logs.push(line); },
      moviesGraphicsCall(command, args) { commands[command] = (commands[command] ?? 0) + 1; if(command.startsWith("upload"))lastUploads[command]=args.bytes.byteLength; return host.moviesGraphicsCall(command, args); },
      fixtureReadback(mode) {
        const image = client.readPixels();
        const sample = (x, y) => Array.from(image.rgba.slice((y * image.width + x) * 4, (y * image.width + x) * 4 + 4));
        if(mode>=300){
          const q=rgba=>targetFormat===25?quantize(rgba):rgba;
          let destination=q([96,160,192,144]),source=[96,64,32,128],sf,df;
          if(mode>=1200){destination=mode===1200?q([17,34,51,32]):additiveDestination;source=[19,33,11,32];sf=df=2;}
          else if(mode>=1000){const n=mode===1121?0:mode-1000;sf=Math.floor(n/11)+1;df=n%11+1;}
          else{const eq=Math.floor((mode-300)/3),pair=(mode-300)%3;[sf,df]=[[2,2],[1,2],[1,3]][pair];
            if(eq===0)source=source.map((v,i)=>v*[64,128,192,160][i]/255);
            else if(eq===1)source=[32,96,160,128*160/255];else if(eq===3)source[3]=160;
          }
          const f=(code,i)=>{switch(code){case 1:return 0;case 2:return 1;case 3:return source[i]/255;case 4:return 1-source[i]/255;case 5:return source[3]/255;case 6:return 1-source[3]/255;case 7:return destination[3]/255;case 8:return 1-destination[3]/255;case 9:return destination[i]/255;case 10:return 1-destination[i]/255;case 11:return i===3?1:Math.min(source[3]/255,1-destination[3]/255);default:throw Error('Invalid reference factor');}};
          const expected=q(source.map((v,i)=>Math.round(Math.max(0,Math.min(255,v*f(sf,i)+destination[i]*f(df,i))))));
          if(mode>=1200)additiveDestination=expected;const rgba=sample(68,44);
          if(rgba.some((v,i)=>Math.abs(v-expected[i])>2))throw new Error(`Classic blend case${mode} pair${sf}/${df}: ${rgba} expected ${expected}`);
          checks.push({label:`Classic ADD blend case${mode}`,factors:[sf,df],rgba,expected});return true;
        }
        if(mode>=80){
          const q=rgba=>targetFormat===25?quantize(rgba):rgba;
          let expected=mode===87?multiplyDestination:q([192,160,128,255]);const alpha=mode===80?0:mode===82?255:mode>=86?192:128;
          const src=mode===84?[192,128,64,alpha]:[64,128,192,alpha];
          if(mode!==85)expected=q(expected.map((v,i)=>Math.round(v*src[i]/255)));
          multiplyDestination=expected;const rgba=sample(68,44);
          if(rgba.some((v,i)=>Math.abs(v-expected[i])>2))throw new Error(`Destination multiply case${mode}: ${rgba} expected ${expected}`);
          checks.push({label:`DiffuseRGB textureAlpha destination multiply case${mode}`,rgba,expected});return true;
        }
        if(mode>=70){
          const q=rgba=>targetFormat===25?quantize(rgba):rgba;
          let expected=mode===72?sourceColorDestination:q([17,34,51,96]);const src=[64,128,192,128];
          for(let i=0;i<(mode===71?2:1);i++)expected=src.map((v,c)=>Math.round(v*v/255+expected[c]*(1-v/255)));
          expected=q(expected);sourceColorDestination=expected;const rgba=sample(68,44);
          if(rgba.some((v,i)=>Math.abs(v-expected[i])>2))throw new Error(`Source-color case${mode}: ${rgba} expected ${expected}`);
          checks.push({label:`Diffuse source-color blend case${mode}`,rgba,expected});return true;
        }
        if(mode>=60){
          const q=rgba=>targetFormat===25?quantize(rgba):rgba;
          if(mode!==65)factorDestination=q([17,34,51,255]);
          const a=mode===62?0:mode===63?1:128/255*128/255;
          let expected=mode===66?[18,52,86,255]:mode<62?[64,32,16,Math.round(a*255)]:[64,32,16,a*255].map((v,i)=>Math.round(v*a+factorDestination[i]*(1-a)));
          expected=q(expected);factorDestination=expected;
          const rgba=sample(68,44);if(rgba.some((v,i)=>Math.abs(v-expected[i])>2))throw new Error(`Texture factor case${mode}: ${rgba} expected ${expected}`);
          checks.push({label:`Texture factor equation case${mode}`,rgba,expected});return true;
        }
        if(mode===55||mode===56){
          const expected=targetFormat===25?quantize([89.3529411765,119.4705882353,0,128]):[89.3529411765,119.4705882353,0,128],rgba=sample(68,44);
          if(rgba.some((v,i)=>Math.abs(v-expected[i])>2))throw new Error(`FVF104 perspective/default diffuse: ${rgba} expected ${expected}`);
          if(lastUploads.uploadVertices!==84||lastUploads.uploadIndices!==6)throw new Error('FVF104 must expand3x24B to3x28B host vertices');
          checks.push({label:mode===55?'FVF104 perspective UV and default white diffuse/alpha':'Bound FVF144 perspective UV with stream/base/start offsets',rgba,vertexBytes:84});return true;
        }
        if(mode===54){
          const expected=targetFormat===25?quantize([51,102,153,255]):[51,102,153,255],rgba=sample(95,64);
          if(rgba.some((v,i)=>Math.abs(v-expected[i])>1))throw new Error(`Sliced quad: ${rgba} expected ${expected}`);
          if(lastUploads.uploadVertices!==96||lastUploads.uploadIndices!==12)throw new Error(`Unexpected quad upload sizes ${JSON.stringify(lastUploads)}`);
          checks.push({label:'Referenced quad ignores unused NaNs in4MiB VB',rgba,vertexBytes:96,indexBytes:12});return true;
        }
        if(mode>=50){
          const expected=mode===50?[17,34,51,136]:mode===51?[85,0,170,170]:mode===52?[0,0,0,0]:[85,0,170,255],wanted=targetFormat===25?quantize(expected):expected,rgba=sample(68,44);
          if(rgba.some((v,i)=>Math.abs(v-wanted[i])>1))throw new Error(`Packed texture case${mode}: ${rgba} expected ${wanted}`);
          checks.push({label:mode===50?'A4R4G4B4 sampled texture':mode===51?'DXT3 sampled texture':`DXT1 ${mode===52?'transparent':'opaque'} sampled texture`,rgba,expected:wanted});return true;
        }
        if(mode>=40){
          const black=[0,0,0,255],red=[255,0,0,255],green=[0,255,0,255],blue=[0,0,255,255],white=[255,255,255,255],yellow=[255,255,0,255];
          const center=mode===40||mode===43||mode===44?red:mode===41?[127,0,128,191]:mode===42?green:mode===47?yellow:blue;
          const outer=mode<=41?red:mode<=45?green:mode===46?white:yellow;
          const points=[[68,44,center],[40,30,outer],[80,44,outer],[68,50,outer],[0,0,mode===47?yellow:black]];
          for(const [x,y,expected] of points){const wanted=targetFormat===25?quantize(expected):expected,rgba=sample(x,y);
            if(rgba.some((v,i)=>Math.abs(v-wanted[i])>1))throw new Error(`Scissor case${mode} at${x},${y}: ${rgba} expected ${wanted}`);
            checks.push({label:`Scissor case ${mode}`,x,y,rgba});
          }
          return true;
        }
        if(mode>=27){
          const white=[255,255,255,3],red=[255,0,0,128],blue=[0,0,255,128];
          let clear=mode===27?[0,0,255,255]:mode>=31?(mode===35?[0,0,0,0]:mode===36||mode===37?[17,34,51,255]:[0,0,0,255]):null;
          if(clear)canonicalBlend=targetFormat===25?quantize(clear):clear;
          let colors=mode===29?[[0,255,0,0]]:mode===30?[[0,255,0,255]]:mode===31||mode===32?[white,white]:mode===33?[red,blue]:mode===34?[blue,red]:mode>=38?Array.from({length:64},()=>white):[red];
          const starting=canonicalBlend.slice();let strict=starting.slice();
          if(mode!==36&&mode!==37)for(const src of colors){
            canonicalBlend=blend(canonicalBlend,src);
            strict=targetFormat===25?quantize(blend(strict,src)):blend(strict,src);
            if(targetFormat===25&&(mode===32||mode===39))canonicalBlend=quantize(canonicalBlend);
          }
          if(targetFormat===25)canonicalBlend=quantize(canonicalBlend);
          const rgba=sample(68,44),expected=canonicalBlend;
          if(rgba.some((v,i)=>Math.abs(v-expected[i])>2))throw new Error(`RGBA8 blend/draw resolve case${mode}: ${rgba} expected ${expected}`);
          checks.push({label:`RGBA8 blend/draw resolve case ${mode}`,x:68,y:44,rgba,expected:expected.slice()});
          blendComparisons.push({mode,triangles:colors.length,separateDraws:mode===32||mode===39,actual:rgba,declaredRGBA8PerDraw:expected.slice(),illustrativePerFragment5Bit:strict});
          const outside=sample(19,19),outsideExpected=targetFormat===25?quantize(clear??[0,0,255,255]):(clear??[0,0,255,255]);
          if(outside.some((v,i)=>Math.abs(v-outsideExpected[i])>1))throw new Error('Blend resolve modified untouched pixel');
          checks.push({label:'blend untouched pixel',x:19,y:19,rgba:outside});return true;
        }
        if(mode>=16){
          const depthExpected=[[255,0,0,255],[0,0,255,255],[0,0,255,255],[255,255,0,255],[255,255,0,255],[0,255,255,255],[255,0,255,255]];
          const points=mode>=20 ? [[`Depth comparison/write case ${mode}`,68,44,depthExpected[mode-20]]] : mode===16 ? Array.from({length:16},(_,i)=>['RHW half-pixel texel',20+i%4,20+Math.floor(i/4),[20+(i%4)*60,20+Math.floor(i/4)*60,40,255]]) :
            [[mode===17?'RHW screen-linear diffuse':mode===18?'RHW perspective UV':'RHW depth bypasses viewport transform',68,44,mode===17?[96,96,64,255]:mode===18?[89.3529411765,119.4705882353,0,255]:[255,255,255,255]]];
          points.push(['RHW outside geometry',19,19,[17,34,51,255]]);
          for(const [label,x,y,expected] of points){
            if(targetFormat===25)for(let i=0;i<4;i++){const scale=i===3?1:31;expected[i]=Math.round(Math.floor(expected[i]/255*scale+.5)/scale*255);}
            const rgba=sample(x,y);if(rgba.some((v,i)=>Math.abs(v-expected[i])>2))throw new Error(`${label}: ${rgba} expected ${expected}`);
            checks.push({label,x,y,rgba});
          }
          return true;
        }
        for (const [label, x, y, expected] of [[mode ? `native texture/sampler case ${mode}` : 'native indexed draw',95,64,mode>=8?(mode>=12?[0,0,255,127]:[17,34,51,255]):mode===1?[51,34,17,255]:mode===2?[171,205,239,255]:mode===3?[128,0,128,255]:mode===5?[255,0,0,255]:mode>=4?[0,0,255,255]:[0,255,0,255]],['native clear',0,0,[17,34,51,255]]]) {
          if(targetFormat===25)for(let i=0;i<4;i++){const scale=i===3?1:31;expected[i]=Math.round(Math.floor(expected[i]/255*scale+.5)/scale*255);}
          const rgba = sample(x,y);if(rgba.some((value,i)=>Math.abs(value-expected[i])>1))throw new Error(`${label}: ${rgba} expected ${expected}`);
          checks.push({label,x,y,rgba});
        }
        return true;
      }});
    const result = module.callMain([String(targetFormat)]);
    if (result !== 0 && result !== undefined) throw new Error(`Native fixture exit ${result}`);
    if (!commands.drawIndexed || !commands.present || checks.length !== 296) throw new Error(`Native GPU path did not complete: checks=${checks.length}/296 drawIndexed=${commands.drawIndexed??0} present=${commands.present??0}`);
    self.postMessage({type:'result',result:{status:'passed',label:'Synthetic native D3D9 ABI fixture, not game rendering',targetFormat,capabilities:client.capabilities,checks,commands,logs,blendComparisons,blendModel:'RGBA8 hardware intermediate; RGB5/A1 UNORM8 conversion after each guest draw; not legacy-driver bit-exact',transport:'C COM vtables→EM_JS→D3D host→synchronous guest client→dedicated WebGPU worker→RAF bitmap acknowledgement'}});
  } catch (error) { self.postMessage({type:'error',message:String(error?.stack||error)}); }
  finally { host?.close();client?.close(); }
};
