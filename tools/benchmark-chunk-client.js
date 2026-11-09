// Minimal offline Minecraft 26.3 benchmark client. Connect only through the
// loopback SSH tunnel created by benchmark-chunk-host.js; never production.
// Packet layouts checked against minecraft-data PR #1345, commit 312f3cada17a7d9e2e1a1834fe26f17296163096.
// No rendering, physics, terrain decoding or gameplay AI is simulated.
'use strict';
const net = require('node:net');
const zlib = require('node:zlib');
function vi(n) { const a=[]; do { let b=n&127; n >>>=7; a.push(b|(n?128:0)); } while(n); return Buffer.from(a); }
function readvi(b, p=0) { let n=0; for(let i=0;i<5;i++) { if(p+i>=b.length)return null; const v=b[p+i]; n|=(v&127)<<(7*i); if(!(v&128))return [n>>>0,p+i+1]; } throw Error('invalid VarInt'); }
function str(s) { const b=Buffer.from(s); return Buffer.concat([vi(b.length),b]); }
function f32(n) { const b=Buffer.alloc(4); b.writeFloatBE(n); return b; }
class BenchmarkClient {
  constructor(port, report=()=>{}, requestedView=32) {
    if(!Number.isInteger(requestedView)||requestedView<2||requestedView>127)throw Error('client view request must fit the protocol signed byte (2..127)');
    this.requestedView=requestedView;
    this.report=report; this.state='login'; this.compression=-1; this.buffer=Buffer.alloc(0);
    this.bytes=0; this.chunks=0; this.loaded=new Set(); this.peakLoaded=0; this.samples=[]; this.errors=[]; this.pos=null;
    this.socket=net.createConnection({host:'127.0.0.1',port},()=>{
      const portBytes=Buffer.alloc(2);portBytes.writeUInt16BE(25665);
      this.send(0,Buffer.concat([vi(777),str('localhost'),portBytes,vi(2)]));
      this.send(0,Buffer.concat([str('BenchClient'),Buffer.alloc(16)]));
    });
    this.socket.on('data',b=>{try {this.bytes+=b.length;this.buffer=Buffer.concat([this.buffer,b]);this.parse();}catch(e){this.fail(e.message);}});
    this.socket.on('error',e=>this.fail(e.message));
    this.socket.on('close',()=>{this.closed=true;clearInterval(this.timer);this.report({event:'client_closed',errors:this.errors});});
    this.timer=setInterval(()=>{if(this.state==='play'&&!this.socket.destroyed){this.send(0x0d); if(Date.now()-(this.subscribed||0)>4000){this.send(0x17,Buffer.from([1,0]));this.subscribed=Date.now();}}},50);
  }
  fail(s) {this.errors.push(s);this.report({event:'client_error',message:s});this.socket.destroy();}
  send(id, data=Buffer.alloc(0)) {
    let body=Buffer.concat([vi(id),data]);
    if(this.compression>=0)body=body.length>=this.compression?Buffer.concat([vi(body.length),zlib.deflateSync(body)]):Buffer.concat([vi(0),body]);
    this.socket.write(Buffer.concat([vi(body.length),body]));
  }
  settings() {return Buffer.concat([str('en_us'),Buffer.from([this.requestedView,0,1,127,1,0,1,0])]);}
  parse() {
    while(this.buffer.length){const v=readvi(this.buffer);if(!v)return;const [length,p]=v;if(length>16*1024*1024)throw Error('oversized packet');if(this.buffer.length<p+length)return;
      let b=this.buffer.subarray(p,p+length);this.buffer=this.buffer.subarray(p+length);
      if(this.compression>=0){const [size,q]=readvi(b);b=size?zlib.inflateSync(b.subarray(q),{maxOutputLength:16*1024*1024}):b.subarray(q);if(size&&b.length!==size)throw Error('bad compressed packet');}
      const [id,q]=readvi(b);this.packet(id,b.subarray(q));
    }
  }
  packet(id,b) {
    if(this.state==='login'){
      if(id===3)this.compression=readvi(b)[0];
      else if(id===2){this.send(3);this.state='configuration';this.send(0,this.settings());}
      else if(id===0||id===1)throw Error('login rejected or online-mode authentication requested');
      return;
    }
    if(this.state==='configuration'){
      if(id===3){this.send(3);this.state='play';this.report({event:'client_play'});}
      else if(id===4)this.send(4,b);
      else if(id===5)this.send(5,b);
      else if(id===15)this.send(7,vi(0));
      else if(id===20)this.send(9);
      else if(id===2)throw Error('configuration disconnected');
      return;
    }
    if(id===0x2d)this.send(0x1c,b);
    else if(id===0x3e)this.send(0x2d,b);
    else if(id===0x0b)this.send(0x0b,f32(64));
    else if(id===0x49){
      const [teleport,q]=readvi(b), flags=b.readUInt32BE(q+56);
      const previous=this.pos||{x:0,y:0,z:0,yaw:0,pitch:0};
      this.pos={x:b.readDoubleBE(q)+(flags&1?previous.x:0),y:b.readDoubleBE(q+8)+(flags&2?previous.y:0),z:b.readDoubleBE(q+16)+(flags&4?previous.z:0),yaw:b.readFloatBE(q+48)+(flags&8?previous.yaw:0),pitch:b.readFloatBE(q+52)+(flags&16?previous.pitch:0)};
      const pos=Buffer.alloc(32);pos.writeDoubleBE(this.pos.x,0);pos.writeDoubleBE(this.pos.y,8);pos.writeDoubleBE(this.pos.z,16);pos.writeFloatBE(this.pos.yaw,24);pos.writeFloatBE(this.pos.pitch,28);
      this.send(0,Buffer.concat([vi(teleport),pos]));this.send(0x2c);
    }
    else if(id===0x2e){this.chunks++;this.loaded.add(`${b.readInt32BE(0)},${b.readInt32BE(4)}`);this.peakLoaded=Math.max(this.peakLoaded,this.loaded.size);}
    else if(id===0x26){this.loaded.delete(`${b.readInt32BE(4)},${b.readInt32BE(0)}`);}
    else if(id===0x61||id===0x71)this.report({event:id===0x61?'view_distance':'simulation_distance',value:readvi(b)[0]});
    else if(id===0x32){let p=5;const [count,q]=readvi(b,p);p=q;for(let i=0;i<count;i++){const [n,s]=readvi(b,p);p=s+n;}let v=readvi(b,p);p=v[1];v=readvi(b,p);const view=v[0];v=readvi(b,v[1]);this.report({event:'login_distances',view,simulation:v[0]});}
    else if(id===0x1e){const [count,q]=readvi(b);const values=[];for(let i=0;i<count;i++)values.push(Number(b.readBigInt64BE(q+8*i)));this.samples.push(values);}
    else if(id===0x20){
      if(b.includes(Buffer.from('multiplayer.disconnect.server_shutdown')))this.report({event:'server_shutdown'});
      else throw Error('server disconnected benchmark client: '+b.toString('hex').slice(0,180));
    }
  }
  snapshot() {return {bytes:this.bytes,chunks:this.chunks,loaded:this.loaded.size,peakLoaded:this.peakLoaded,samples:this.samples.splice(0),errors:[...this.errors],position:this.pos};}
  close() {clearInterval(this.timer);this.socket.destroy();}
}
module.exports={BenchmarkClient,vi,readvi};
