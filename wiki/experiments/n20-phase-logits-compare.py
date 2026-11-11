import array,math,struct,sys
from pathlib import Path
p=Path(sys.argv[1]) if len(sys.argv) == 2 else Path('/tmp/n20-hybrid-identity-20261110')
vocab=248320; steps=17
for q in ('j03','j10','t257','t1024'):
 err=(p/(q+'-candidate.err')).read_text()
 if 'n20 phase attention: flash' not in err or 'n20 phase attention: one-token non-FA' not in err: raise ValueError('missing path '+q)
 tokens=struct.unpack('<17i',(p/(q+'.tokens')).read_bytes())
 maxdiff=0.; sqerr=0.; sqbase=0.; topagree=0; minmargin=math.inf; firstmax=0.; total=0
 with (p/(q+'-before.logits')).open('rb') as a,(p/(q+'-candidate.logits')).open('rb') as b:
  for step in range(steps):
   va=array.array('f'); vb=array.array('f'); va.fromfile(a,vocab); vb.fromfile(b,vocab)
   best=-math.inf; second=-math.inf; top=-1; candidate_best=-math.inf; candidate_top=-1; stepmax=0.
   for i,(x,y) in enumerate(zip(va,vb)):
    if not math.isfinite(x) or not math.isfinite(y): raise ValueError('nonfinite logit')
    d=abs(x-y); stepmax=max(stepmax,d); sqerr+=d*d; sqbase+=x*x
    if x>best: second=best; best=x; top=i
    elif x>second: second=x
    if y>candidate_best: candidate_best=y; candidate_top=i
   if step==0: firstmax=stepmax
   maxdiff=max(maxdiff,stepmax); topagree+=top==candidate_top; minmargin=min(minmargin,best-second)
   if top != tokens[step]: raise ValueError('baseline token mismatch')
  if a.read(1) or b.read(1): raise ValueError('unexpected logit size')
 print(q,'steps=',steps,'first_max_abs=',firstmax,'max_abs=',maxdiff,'normalized_rms=',math.sqrt(sqerr/max(sqbase,1e-30)),'top_agreement=',topagree,'min_control_margin=',minmargin,flush=True)
