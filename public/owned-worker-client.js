// Worker-compatible facade; install before Emscripten constructs its pool.
export function installOwnedWorkerClient(port,{scope=globalThis}={}){
 if(!port||typeof port.postMessage!=='function')throw Error('Owned worker port missing');
 const NativeWorker=scope.Worker,workers=new Map();let serial=0,closed=false;
 function emit(worker,type,event){if(worker.closed)return;event.type=type;event.target=worker;worker['on'+type]?.(event);for(const listener of [...(worker.listeners.get(type)||[])]){if(typeof listener==='function')listener.call(worker,event);else listener.handleEvent(event);}}
 class OwnedWorker{
  constructor(url,options={}){if(closed)throw Error('Owned worker transport closed');this.id=++serial;this.closed=false;this.listeners=new Map();workers.set(this.id,this);port.postMessage({op:'create',id:this.id,url:String(url),options:{name:options.name,type:options.type}});}
  postMessage(message,transfer=[]){if(this.closed||closed)throw Error('Owned worker terminated');if(!Array.isArray(transfer))transfer=transfer.transfer||[];port.postMessage({op:'post',id:this.id,message,transfer},transfer);}
  terminate(){if(this.closed)return;this.closed=true;workers.delete(this.id);if(!closed)port.postMessage({op:'terminate',id:this.id});}
  addEventListener(type,listener){if(!this.listeners.has(type))this.listeners.set(type,new Set());this.listeners.get(type).add(listener);}
  removeEventListener(type,listener){this.listeners.get(type)?.delete(listener);}
 }
 port.onmessage=({data})=>{const worker=workers.get(data?.id);if(!worker||closed)return;if(data.op==='message')emit(worker,'message',{data:data.data});else if(data.op==='error')emit(worker,'error',{message:data.message,preventDefault(){}});};
 port.onmessageerror=()=>{for(const worker of workers.values())emit(worker,'messageerror',{});};port.start?.();scope.Worker=OwnedWorker;
 return {close(){if(closed)return;for(const worker of [...workers.values()])worker.terminate();closed=true;port.close();if(scope.Worker===OwnedWorker)scope.Worker=NativeWorker;},Worker:OwnedWorker};
}
