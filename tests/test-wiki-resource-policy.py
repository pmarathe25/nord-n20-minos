"""Swap-aware admission/yield hysteresis, with entirely synthetic samples."""
import importlib.machinery
from pathlib import Path

m=importlib.machinery.SourceFileLoader('resource_policy',str(Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-resource-policy')).load_module()
normal={'MemAvailable':4000000,'SwapFree':6000000,'memory_some':0,'memory_full':0,
        'io_full':0,'java_memsw':0,'old_model':False,'old_bridge':False}
state={}
for now in range(30):assert m.update(state,normal,'launcher',now)=='healthy admission window'
assert not m.update(state,normal,'launcher',30)
# Sub-1GiB RAM plus adequate swap is intentionally usable.
paging=dict(normal,MemAvailable=800*1024,SwapFree=3*1024**2,java_memsw=5*1024**3)
assert not m.reason(paging,True)
paging['MemAvailable']=500*1024
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
assert m.update(state,dict(normal,MemAvailable=255*1024),'running',220)=='emergency RAM reserve'
for field,value,checks,label in (
    ('MemAvailable',350*1024,5,'sustained low RAM reserve'),
    ('SwapFree',700*1024,3,'swap reserve for Minecraft'),
    ('io_full',30,10,'sustained storage pressure')):
    s={};sample=dict(normal,**{field:value})
    for now in range(checks-1):assert not m.update(s,sample,'running',now)
    assert m.update(s,sample,'running',checks-1)==label
assert m.reason(dict(normal,old_model=True),True)
assert m.reason(dict(normal,old_bridge=True),True,True)
assert not m.reason(dict(normal,old_bridge=True),True,False)
assert m.reason(dict(normal,MemAvailable=768*1024-1),True)
print('SWAP_AWARE_MINECRAFT_FIRST_HYSTERESIS_PASSED')
