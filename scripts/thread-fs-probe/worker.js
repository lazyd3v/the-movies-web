const NativeWorker=Worker;
self.Worker=class extends NativeWorker {constructor(url,options){super(url,options);this.addEventListener('error',event=>postMessage({type:'result',passed:false,error:`Pthread worker ${url}: ${event.message}`}));this.addEventListener('messageerror',()=>postMessage({type:'result',passed:false,error:`Pthread worker message error: ${url}`}));}};
importScripts('/range-file.js','/stored-archive-fs.js','./probe.js');
onmessage=async({data})=>{try{
 const {installOwnedWorkerClient}=await import('/owned-worker-client.js');
 installOwnedWorkerClient(data.port);
 const index=await (await fetch('/game/file-index.json')).json();
 let report;const runtime=await createThreadFSProbe({noInitialRun:true,mainScriptUrlOrBlob:new URL('./probe.js',self.location.href).href,print(text){if(text.startsWith('RESULT '))report=JSON.parse(text.slice(7));else postMessage({type:'log',text});},printErr:text=>postMessage({type:'log',text}),locateFile:path=>new URL(path,self.location.href).href});
 const archive=await MoviesStoredArchiveFS.mount(runtime.FS,{index,url:new URL('/game/'+index.archive,self.location.href).href,path:'/game'});
 const code=runtime.callMain([String(data.mode)]);
 postMessage({type:'result',...report,exitCode:code,rangeStats:archive.stats});
}catch(error){postMessage({type:'result',passed:false,error:String(error)});}};
