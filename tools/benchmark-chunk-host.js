// Run from the repository root. The client runs on the workstation, not phone.
'use strict';
const {spawn}=require('node:child_process');
const {createInterface}=require('node:readline');
const {BenchmarkClient}=require('./benchmark-chunk-client');
const args=process.argv.slice(2);
let requestedView=32;
const requestIndex=args.indexOf('--client-view');
if(requestIndex!==-1){requestedView=Number(args[requestIndex+1]);args.splice(requestIndex,2);}
if(!Number.isInteger(requestedView)||requestedView<2||requestedView>127)throw Error('--client-view must be 2..127');
const remoteScript=process.env.N20_BENCHMARK_SCRIPT||'/run/n20/chunk-benchmark/benchmark-chunk-server.py';
if(!/^\/run\/n20\/[a-zA-Z0-9_-]+\/benchmark-chunk-server\.py$/.test(remoteScript))throw Error('invalid remote script path');
const phone=process.env.N20_BENCHMARK_HOST||'192.168.137.2';
if(require('node:net').isIP(phone)!==4)throw Error('N20_BENCHMARK_HOST must be an IPv4 address');
const ssh=spawn('ssh',['-i','local/secrets/n20_usb_key','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes',
  '-L','127.0.0.1:25666:127.0.0.1:25665','root@'+phone,
  'python3',remoteScript,'--stop-production',...args]);
let client=null,baseline=null,phase=null,activeCase=null;
const statusTimer=setInterval(()=>{if(client&&!ssh.stdin.destroyed)ssh.stdin.write(JSON.stringify({case:activeCase,chunks:client.chunks,loaded:client.loaded.size,errors:client.errors,closed:!!client.closed})+'\n');},1000);
function output(o){console.log(JSON.stringify(o));}
ssh.stderr.on('data',b=>process.stderr.write(b));
const reader=createInterface({input:ssh.stdout});
reader.on('line',line=>{
  let event;try{event=JSON.parse(line);}catch{process.stderr.write(line+'\n');return;}
  if(event.event==='ready'){activeCase=event.case;const request=requestIndex===-1&&event.view>32?127:requestedView;output({event:'client_request',view:request});client=new BenchmarkClient(25666,output,request);}
  if(event.event==='phase'){baseline=client.snapshot();phase=event.phase;}
  if(event.event==='phase_result'){
    const current=client.snapshot();const samples=current.samples;
    // Retain raw dimensions until their semantics are verified against the JAR.
    function statistics(input){
      const values=input.filter(Number.isFinite).sort((a,b)=>a-b);
      return {count:values.length,mean_ms:values.reduce((a,b)=>a+b,0)/values.length,
        p50_ms:values[Math.floor(values.length*.5)],p95_ms:values[Math.min(values.length-1,Math.ceil(values.length*.95)-1)],
        p99_ms:values[Math.min(values.length-1,Math.ceil(values.length*.99)-1)],max_ms:values.at(-1),
        over_50ms_pct:100*values.filter(v=>v>50).length/values.length};
    }
    const dimensions=[];
    for(let i=0;i<Math.max(0,...samples.map(s=>s.length));i++)dimensions.push(statistics(samples.map(s=>s[i]/1e6)));
    // Installed TpsDebugDimensions order: FULL_TICK, TICK_SERVER_METHOD,
    // SCHEDULED_TASKS, IDLE. Exclude intentional sleep from tick work.
    const active=statistics(samples.filter(s=>s.length===4).map(s=>(s[0]-s[3])/1e6));
    event.client={phase,received_mib:(current.bytes-baseline.bytes)/2**20,chunks_received:current.chunks-baseline.chunks,
      loaded_chunks:current.loaded,peak_loaded:current.peakLoaded,active_tick:active,debug_dimensions:dimensions,errors:current.errors,position:current.position};
  }
  if(event.event==='case_end'){client?.close();client=null;}
  output(event);
});
ssh.on('exit',code=>{clearInterval(statusTimer);client?.close();process.exitCode=code||0;});
process.on('SIGINT',()=>{client?.close();ssh.kill();});
