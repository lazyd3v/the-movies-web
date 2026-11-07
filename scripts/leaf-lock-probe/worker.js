importScripts('./probe.js');
onmessage=async({data})=>{try{
 const {installOwnedWorkerClient}=await import('/owned-worker-client.js');
 installOwnedWorkerClient(data.port);
 const runtime=await createLeafLockProbe({noInitialRun:true,mainScriptUrlOrBlob:new URL('./probe.js',self.location.href).href,locateFile:p=>new URL(p,self.location.href).href,probeControl:descriptor=>postMessage({type:'control',...descriptor}),printErr:text=>postMessage({type:'log',text})});
 runtime.callMain([]);
 postMessage({type:'unexpected-return'});
}catch(error){postMessage({type:'parent-error',error:String(error)});}};
