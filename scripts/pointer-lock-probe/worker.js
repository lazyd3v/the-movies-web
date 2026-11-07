import '../../window-input.js';
import createPointerProbe from './native.js';
let module,cursor;
onmessage=async({data})=>{try{
 if(data.type==='init'){cursor=new MoviesPointerSnapshot(data.buffer);module=await createPointerProbe({moviesCursorPosition:()=>cursor.readBlocking(),moviesSetCursorPosition:(x,y)=>cursor.warp(x,y)});if(!module._probe_init())throw Error('Native init failed');postMessage({type:'ready'});return;}
 const warped=module._probe_warp(data.x??512,data.y??384),error=module._probe_error(),read=module._probe_read();
 postMessage({type:'reply',id:data.id,warped,error,read,point:read?{x:module._probe_x(),y:module._probe_y()}:null});
 }catch(error){postMessage({type:'error',message:String(error.stack||error)});}};
