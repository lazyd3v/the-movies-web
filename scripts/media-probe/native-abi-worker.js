'use strict';
importScripts('/media-service-client.js', 'native-abi-module.js');
onmessage = async ({data}) => {
  let client;
  const logs = [], operations = {create:0,configure:0,release:0};
  try {
    client = await MoviesMediaServiceClient.connect(data.service);
    const provider = client.provider();
    const runtime = await createNativeMediaProbe({noInitialRun:true, mainScriptUrlOrBlob:new URL('native-abi-module.js',location.href).href,
      moviesMediaCreateWriter() { const token = provider.moviesMediaCreateWriter(); if (token) operations.create++; return token; },
      moviesMediaConfigureWriter(token, profile) { const ok = provider.moviesMediaConfigureWriter(token,profile); if (ok) operations.configure++; return ok; },
      moviesMediaReleaseWriter(token) { const ok = provider.moviesMediaReleaseWriter(token); if (ok) operations.release++; return ok; },
      print:text=>logs.push(text), printErr:text=>logs.push(text)});
    const code = runtime.callMain([]);
    if (code !== 0 || operations.create !== 1 || operations.configure !== 1 || operations.release !== 1) throw Error('Native COM service calls did not complete exactly once');
    postMessage({ok:true,label:'Synthetic native COM ABI fixture, not game playback',operations,logs,
      transport:'Native COM vtables → EM_JS → synchronous client → real verified WMV2/WMAv2 codec service'});
  } catch(error) { postMessage({ok:false,error:String(error),operations,logs}); }
  finally { client?.close(); }
};
