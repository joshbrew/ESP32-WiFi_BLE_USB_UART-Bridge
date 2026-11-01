import http from 'node:http';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

export function checkHealth({port=Number(process.env.PORT)||8080,key=process.env.INTERNAL_RELAY_KEY,timeout=2500}={}){
  return new Promise((resolve,reject)=>{
    if(!key){reject(Error('Relay health key is missing'));return}
    const request=http.get({hostname:'127.0.0.1',port,path:'/healthz',headers:{'X-Relay-Key':key},agent:false,timeout},response=>{
      let body='';
      response.on('data',chunk=>{body+=chunk;if(Buffer.byteLength(body)>65536)request.destroy(Error('Health response exceeds limit'))});
      response.on('end',()=>{
        let data;try{data=JSON.parse(body)}catch{reject(Error('Relay health response is invalid'));return}
        if(response.statusCode!==200||data.ok!==true||data.relay?.http!=='ready'){reject(Error('Relay HTTP service is not ready'));return}
        resolve(data);
      });
      response.on('error',()=>reject(Error('Relay health response was interrupted')));
    });
    request.on('timeout',()=>request.destroy(Error('Health request timed out')));request.on('error',()=>reject(Error('Relay health endpoint is unavailable')));
  });
}

if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
  checkHealth().then(data=>console.log(`Relay HTTP ready. Device link: ${data.device?.phase||'unknown'}.`)).catch(error=>{console.error(error.message);process.exitCode=1});
}
