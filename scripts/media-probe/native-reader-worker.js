'use strict';
importScripts('/media-reader-client.js','native-reader-module.js');
onmessage=async({data})=>{
  let client, result;
  const logs=[];
  try {
    const response=await fetch('reader-audio-only.asf');
    if(!response.ok)throw Error('Reader source HTTP '+response.status);
    const source=new Uint8Array(await response.arrayBuffer());
    const sourceSha256=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',source)),b=>b.toString(16).padStart(2,'0')).join('');
    client=await MoviesMediaReaderClient.connect(data.service);
    const runtime=await createNativeMediaReaderProbe({noInitialRun:true,
      mainScriptUrlOrBlob:new URL('native-reader-module.js',location.href).href,
      ...client.readerProvider(),moviesReaderFixtureBytes:source,
      moviesReaderFixtureResult:value=>{result=value;},print:text=>logs.push(text),printErr:text=>logs.push(text)});
    const code=runtime.callMain([]);
    if(code!==0||!result?.ok)throw Error('Native reader fixture did not complete');
    postMessage({...result,label:'Native IStream/IWMSyncReader fixture, not game playback',sourceBytes:source.length,sourceSha256,logs});
  }catch(error){postMessage({ok:false,error:String(error),logs});}
  finally{client?.close();}
};
