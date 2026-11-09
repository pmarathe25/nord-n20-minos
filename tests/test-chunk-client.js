// Offline protocol regression: compression, fragmented input, chunk lifetime,
// and the extended 26.3 teleport acknowledgement. No device connection.
'use strict';
const assert=require('node:assert/strict');
const net=require('node:net');
const zlib=require('node:zlib');
const {BenchmarkClient,vi,readvi}=require('../tools/benchmark-chunk-client');
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function main(){
  for(const n of [0,127,128,255,777,2147483647])assert.deepEqual(readvi(vi(n)),[n,vi(n).length]);
  assert.equal(readvi(Buffer.from([128])),null);
  assert.throws(()=>readvi(Buffer.alloc(5,128)),/invalid VarInt/);
  assert.throws(()=>new BenchmarkClient(0,()=>{},256),/signed byte/);
  const received=[],events=[];
  let peer,buffer=Buffer.alloc(0),compressed=false;
  const server=net.createServer(socket=>{
    peer=socket;
    socket.on('data',data=>{
      buffer=Buffer.concat([buffer,data]);
      while(buffer.length){const length=readvi(buffer);if(!length)return;const [n,p]=length;if(buffer.length<n+p)return;
        let b=buffer.subarray(p,p+n);buffer=buffer.subarray(p+n);
        if(compressed){const [size,q]=readvi(b);b=size?zlib.inflateSync(b.subarray(q)):b.subarray(q);}
        received.push(b);
      }
    });
  });
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const client=new BenchmarkClient(server.address().port,o=>events.push(o));
  const deadline=setTimeout(()=>{throw Error('protocol test timeout');},5000);
  async function send(id,data=Buffer.alloc(0),split=false){
    let body=Buffer.concat([vi(id),data]);
    if(compressed)body=Buffer.concat([vi(body.length),zlib.deflateSync(body)]);
    const frame=Buffer.concat([vi(body.length),body]);
    if(split){peer.write(frame.subarray(0,2));await delay(5);peer.write(frame.subarray(2));}else peer.write(frame);
    await delay(20);
  }
  try{
    while(received.length<2)await delay(5);
    assert.equal(readvi(received[0],1)[0],777);
    await send(3,vi(1));compressed=true;
    await send(2,Buffer.alloc(0),true);
    assert.equal(client.state,'configuration');
    await send(15,vi(0));
    await send(3);
    assert.equal(client.state,'play');
    const position=Buffer.alloc(60);position.writeDoubleBE(10,0);position.writeDoubleBE(100,8);position.writeDoubleBE(-20,16);position.writeFloatBE(45,48);position.writeFloatBE(5,52);
    await send(0x49,Buffer.concat([vi(1234),position]),true);
    const ack=received.find(b=>b[0]===0&&readvi(b,1)?.[0]===1234);
    assert.ok(ack,'26.3 teleport acknowledgement');
    const offset=readvi(ack,1)[1];assert.equal(ack.length,offset+32);
    assert.equal(ack.readDoubleBE(offset),10);assert.equal(ack.readFloatBE(offset+24),45);
    const chunk=Buffer.alloc(8);chunk.writeInt32BE(-2,0);chunk.writeInt32BE(3,4);
    await send(0x2e,chunk);assert.equal(client.loaded.size,1);
    const unload=Buffer.alloc(8);unload.writeInt32BE(3,0);unload.writeInt32BE(-2,4);
    await send(0x26,unload);assert.equal(client.loaded.size,0);
    const samples=Buffer.alloc(32);[50000000n,10000000n,1000000n,39000000n].forEach((n,i)=>samples.writeBigInt64BE(n,i*8));
    await send(0x1e,Buffer.concat([vi(4),samples,vi(0)]));
    assert.deepEqual(client.snapshot().samples,[[50000000,10000000,1000000,39000000]]);
    await send(0x20,Buffer.from('multiplayer.disconnect.server_shutdown'));
    assert.ok(events.some(e=>e.event==='server_shutdown'));
    assert.deepEqual(client.errors,[]);
    console.log('chunk benchmark protocol tests passed');
  }finally{clearTimeout(deadline);client.close();peer?.destroy();await new Promise(resolve=>server.close(resolve));}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
