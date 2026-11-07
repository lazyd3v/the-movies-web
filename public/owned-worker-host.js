// The page owns actual workers so it can terminate them even when their logical
// parent is blocked in synchronous Wasm. Ports preserve structured-clone payloads.
export function createOwnedWorkerHost({allowedURL,onError=()=>{},WorkerClass=Worker,Channel=MessageChannel}={}){
 const channel=new Channel(),workers=new Map();let closed=false;
 const expected=new URL(allowedURL,globalThis.location?.href).href;
 function send(message,transfer=[]){if(!closed)channel.port1.postMessage(message,transfer);}
 function fail(id,error){send({op:'error',id,message:String(error?.message||error)});onError(error instanceof Error?error:Error(String(error)));}
 channel.port1.onmessage=({data})=>{
  if(closed)return;
  try{
   const {op,id}=data||{};if(!Number.isSafeInteger(id)||id<1)throw Error('Invalid owned worker ID');
   if(op==='create'){
    if(workers.has(id)||workers.size>=32)throw Error('Owned worker limit or duplicate ID');
    const url=new URL(data.url,expected).href;if(url!==expected)throw Error('Unexpected pthread script URL');
    const options=data.options||{};if(options.type&&options.type!=='classic'&&options.type!=='module')throw Error('Invalid worker type');
    const worker=new WorkerClass(url,{name:options.name||'',type:options.type||'classic'});workers.set(id,worker);
    worker.onmessage=event=>send({op:'message',id,data:event.data},event.ports||[]);
    worker.onerror=event=>fail(id,Error(event.message||'Owned worker failed'));
    worker.onmessageerror=()=>fail(id,Error('Owned worker message decoding failed'));
   }else if(op==='post'){
    const worker=workers.get(id);if(!worker)throw Error('Unknown owned worker');worker.postMessage(data.message,data.transfer||[]);
   }else if(op==='terminate'){
    workers.get(id)?.terminate();workers.delete(id);
   }else throw Error('Unknown owned worker operation');
  }catch(error){fail(data?.id,error);}
 };
 channel.port1.onmessageerror=()=>fail(0,Error('Owned worker transport decoding failed'));
 channel.port1.start?.();
 return {port:channel.port2,get size(){return workers.size;},close(){if(closed)return;closed=true;for(const worker of workers.values())worker.terminate();workers.clear();channel.port1.close();channel.port2.close();}};
}
