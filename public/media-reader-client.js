if(typeof MoviesMediaServiceClient==='undefined')importScripts('/media-service-client.js');
(function(scope){
 class Reader extends MoviesMediaServiceClient{
  constructor(options={}){super({workerURL:'/media-reader-service.js',...options});}
  static async connect(options){const client=new Reader(options);try{await client.ready();return client;}catch(e){client.close();throw e;}}
  readerProvider(){
   const safe=(failure,fn)=>(...args)=>{try{return fn(...args);}catch(e){this.lastError=e;return failure;}};
   return {
    moviesMediaReaderAvailable:safe(false,()=>this.request({op:'capabilities'}).ready===true),
    moviesMediaCreateReader:safe(0,size=>this.request({op:'createReader',size}).token),
    moviesMediaAppendReaderInput:safe(0,(token,offset,bytes)=>this.request({op:'appendInput',token,offset},bytes).acceptedBytes===bytes.length?1:0),
    moviesMediaOpenReader:safe(null,token=>this.request({op:'openReader',token}).metadata),
    moviesMediaReadReaderSample:safe(null,(token,maxBytes)=>this.request({op:'readSample',token,maxBytes})),
    moviesMediaSetReaderRange:safe(0,(token,start100ns,duration100ns=null)=>this.request({op:'setRange',token,start100ns,duration100ns}).seeked?1:0),
    moviesMediaReleaseReader:safe(0,token=>this.request({op:'releaseReader',token}).released?1:0),
    moviesMediaOpenVideo:safe(null,token=>this.request({op:'openVideo',token}).metadata),
    moviesMediaSeekVideo:safe(null,(token,start)=>this.request({op:'seekVideo',token,start}).metadata),
    moviesMediaReadVideoFrame:safe(null,token=>this.request({op:'readFrame',token})),
   };
  }
 }
 scope.MoviesMediaReaderClient=Reader;
})(globalThis);
