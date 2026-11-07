importScripts('thread-module.js');
onmessage=async()=>{
 try{
 const module=await createGuestThreadProbe({mainScriptUrlOrBlob:new URL('thread-module.js',self.location.href).href,print:s=>postMessage({log:s}),printErr:s=>postMessage({log:s}),onAbort:s=>postMessage({error:String(s)})});
 module.callMain([]);postMessage({ok:true,scope:'Actual runtime guest ABI child contexts and pthread synchronization; synthetic start, not original game thread'});
 }catch(e){if(e?.status===0)postMessage({ok:true});else postMessage({error:String(e?.stack||e)});}
};
