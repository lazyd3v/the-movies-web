const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const base=path.resolve('.cache/media-probe');global.self={location:{href:'file://'+base+'/core/package/dist/umd/ffmpeg-core.js'}};
const create=require(base+'/core/package/dist/umd/ffmpeg-core.js');
(async()=>{
 const core=await create({wasmBinary:fs.readFileSync(base+'/core/package/dist/umd/ffmpeg-core.wasm')});let logs=[];core.setLogger(({message})=>logs.push(message));
 core.exec('-encoders');fs.writeFileSync(base+'/encoders.log',logs.join('\n'));console.log(logs.filter(x=>/wmv|wma/.test(x)).join('\n'));
 core.FS.writeFile('frame.yuv',fs.readFileSync(base+'/frame.yuv'));core.FS.writeFile('audio.pcm',fs.readFileSync(base+'/audio.pcm'));
 const args=['-stream_loop','-1','-f','rawvideo','-pixel_format','yuv420p','-video_size','640x480','-framerate','25','-i','frame.yuv','-f','s16le','-ar','44100','-ac','2','-i','audio.pcm','-t','1','-c:v','wmv2','-b:v','1000k','-c:a','wmav2','-b:a','128k','-f','asf','roundtrip.asf'];
 logs=[];core.reset();core.setTimeout(30000);let start=performance.now();let status=core.exec(...args);if(status)throw Error('encode failed '+status+'\n'+logs.join('\n'));
 const output=core.FS.readFile('roundtrip.asf');fs.writeFileSync(base+'/roundtrip.asf',output);const encodeMs=performance.now()-start;
 core.reset();core.setTimeout(30000);status=core.exec('-i','roundtrip.asf','-map','0:v:0','-frames:v','1','-pix_fmt','yuv420p','-f','rawvideo','roundtrip.yuv','-map','0:a:0','-t','1','-c:a','pcm_s16le','-f','s16le','roundtrip.pcm');if(status)throw Error('roundtrip decode failed '+status+'\n'+logs.join('\n'));
 const results={encoder:'wmv2 + wmav2',container:'ASF',input:'Repeated first decoded source frame + first ~1s decoded source audio; NOT re-encoding entire original clip',args,encodeMilliseconds:encodeMs,output:{bytes:output.length,sha256:crypto.createHash('sha256').update(output).digest('hex')},decoded:{}};
 for(const name of ['roundtrip.yuv','roundtrip.pcm']){const data=core.FS.readFile(name);fs.writeFileSync(base+'/'+name,data);results.decoded[name]={bytes:data.length,sha256:crypto.createHash('sha256').update(data).digest('hex')};}
 fs.writeFileSync(base+'/encode.log',logs.join('\n'));fs.writeFileSync(base+'/encode-result.json',JSON.stringify(results,null,2));console.log(results);
})().catch(e=>{console.error(e);process.exitCode=1});
