"""Swap-aware admission/yield hysteresis, with entirely synthetic samples."""
import importlib.machinery
from pathlib import Path

m=importlib.machinery.SourceFileLoader('resource_policy',str(Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-resource-policy')).load_module()
normal={'MemAvailable':4000000,'SwapFree':6000000,'memory_some':0,'memory_full':0,
        'io_full':0,'java_memsw':0,'old_model':False,'old_bridge':False}
state={}
for now in range(30):assert m.update(state,normal,'launcher',now)=='healthy admission window'
assert not m.update(state,normal,'launcher',30)
# Even zero reported available RAM may use swap; cold Java pages do not veto it.
paging=dict(normal,MemAvailable=0,SwapFree=3*1024**2,java_memsw=5*1024**3)
assert not m.reason(paging,True)
admission={}
for now in range(30):assert m.update(admission,paging,'start',now)=='healthy admission window'
assert not m.update(admission,paging,'start',30)
for now in range(31,50):assert not m.update(state,paging,'running',now)
# Transient stalls do not stop the model; clearing resets the counter.
stalled=dict(paging,memory_full=8)
for now in range(50,59):assert not m.update(state,stalled,'running',now)
assert not m.update(state,paging,'running',59)
for now in range(60,69):assert not m.update(state,stalled,'running',now)
assert m.update(state,stalled,'running',69)=='sustained memory pressure'
for now in range(70,189):assert m.update(state,normal,'start',now)=='post-yield cooldown'
for now in range(189,219):assert m.update(state,normal,'start',now)=='healthy admission window'
assert not m.update(state,normal,'start',219)
for now in range(220,250):assert not m.update(state,paging,'running',now)
for field,value,checks,label in (
    ('SwapFree',700*1024,3,'swap reserve for Minecraft'),
    ('io_full',30,10,'sustained storage pressure')):
    s={};sample=dict(normal,**{field:value})
    for now in range(checks-1):assert not m.update(s,sample,'running',now)
    assert m.update(s,sample,'running',checks-1)==label
assert m.reason(dict(normal,old_model=True),True)
assert m.reason(dict(normal,old_bridge=True),True,True)
assert not m.reason(dict(normal,old_bridge=True),True,False)
assert not m.reason(dict(normal,MemAvailable=0),True)
assert m.reason(dict(normal,MemAvailable=0,SwapFree=3*1024**2-1),True)=='combined free-memory reserve'
low_total=dict(normal,MemAvailable=0,SwapFree=800*1024)
s={}
for now in range(2):assert not m.update(s,low_total,'running',now)
assert m.update(s,low_total,'running',2)=='combined free-memory reserve'
print('SWAP_AWARE_MINECRAFT_FIRST_HYSTERESIS_PASSED')
