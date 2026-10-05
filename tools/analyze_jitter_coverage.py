"""Analyze saved NGX jitter samples without touching the game or GPU."""
import hashlib
import argparse
import json
from pathlib import Path

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--high',type=Path,default=root/'evidence/capture-86912-native-dlaa/MhwSrProbe-86912.jsonl')
parser.add_argument('--low',type=Path,default=root/'evidence/capture-86912-low-dlaa/after-baseline.jsonl')
parser.add_argument('--output',type=Path,default=root/'evidence/jitter-coverage-analysis.json')
args=parser.parse_args()
if args.output.resolve() in (args.high.resolve(),args.low.resolve()):parser.error('Output must not replace input logs.')
paths={
    'High':args.high,
    'Low':args.low,
}
def halton(n,base):
    value=0.0;fraction=1.0
    while n:
        fraction/=base;value+=fraction*(n%base);n//=base
    return value
pattern=[(halton(i,2)-0.5,halton(i,3)-0.5) for i in range(1,9)]
report={'method':'Offline analysis of preserved parameter captures; no process access or rendering.',
        'halton_reference':{'bases':[2,3],'indices':[1,8],'center':0.5,'points':pattern},'captures':[]}
for setting,path in paths.items():
    data=path.read_bytes()
    rows=[json.loads(line) for line in data.decode('utf-8-sig').splitlines() if line.strip()]
    values=[(r['jitter_x'],r['jitter_y']) for r in rows if r.get('event')=='evaluate' and r.get('result')==1
        and isinstance(r.get('jitter_x'),(float,int)) and isinstance(r.get('jitter_y'),(float,int))]
    nonzero=[v for v in values if v!=(0,0)]
    indices=[];unmatched=[]
    for v in nonzero:
        i=next((i+1 for i,p in enumerate(pattern) if max(abs(v[j]-p[j]) for j in (0,1))<1e-6),None)
        if i is None:unmatched.append(v)
        else:indices.append(i)
    report['captures'].append({'setting':setting,'path':str(path),
        'sha256':hashlib.sha256(data).hexdigest(),'successful_samples':len(values),
        'zero_samples':len(values)-len(nonzero),'distinct_nonzero_samples':sorted(set(nonzero)),
        'observed_halton_indices':sorted(set(indices)),'matched_nonzero_samples':len(indices),
        'unmatched_nonzero_samples':len(unmatched)})
report['coordinate_inference']={
    'allocation':[2560,1440],'measured_low_active_rectangle':[1920,1080],
    'inferred_low_input_pixel_jitter':[[x*0.75,y*0.75] for x,y in pattern],
    'inferred_quality_input_pixel_jitter':[[x*0.6625,y*0.6625] for x,y in pattern],
    'reason':'MHWSS reads projection jitter and multiplies by allocated dimensions; input pixels use the active dimensions instead.',
    'implication':'If the engine keeps this output-normalized pattern, reducing internal size narrows subpixel coverage. Changing NGX jitter alone cannot expand actual rendered sample positions.',
}
report['limitations']=[
    'Sparse capture does not establish the temporal order or prove an exact eight-frame cycle.',
    'High/Low tags originate from saved configuration/capture sessions; they are not a per-frame projection trace.',
    'The measured 75% ROI is from a later pixel capture; the quality ratio is from the engine pulse.',
    'The converted coverage is an inference from the decoded projection path, not a temporal image-quality validation.',
]
destination=args.output
destination.parent.mkdir(parents=True,exist_ok=True)
destination.write_text(json.dumps(report,indent=2),'utf-8')
print(json.dumps({r['setting']:{k:r[k] for k in ['successful_samples','zero_samples','observed_halton_indices','unmatched_nonzero_samples']} for r in report['captures']},indent=2))
