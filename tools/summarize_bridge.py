"""Summarize an archived bridge preflight log without accessing the game."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('capture',type=Path)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
if args.capture.resolve()==args.output.resolve():parser.error('Output must not replace the raw capture.')
data=args.capture.read_bytes()
rows=[json.loads(line) for line in data.decode('utf-8-sig').splitlines() if line.strip()]
def events(name):return [r for r in rows if r.get('event')==name]
pairs=events('taa_quad_order')
target=[r for r in pairs if r.get('phase')==2]
matched=[r for r in target if r.get('input_rect_observed_at_engine_call') and
    r.get('same_scene_invocation_and_rect') and r.get('taa_scene_invocation')==r.get('quad_scene_invocation') and
    r.get('scope_input_rect')==r.get('source_rect')]
groups=Counter((r.get('phase'),tuple(r.get('source_rect',[])),tuple(r.get('texture_size',[])),tuple(r.get('dispatch',[]))) for r in pairs)
complete=events('sequence_complete')
stopped=events('stopped')
restored=bool(complete and stopped and stopped[-1].get('owned_hooks_restored') and
    not stopped[-1].get('scale_override_pending') and abs(complete[-1].get('active',0)-1)<1e-5)
summary={
    'source':str(args.capture),'sha256':hashlib.sha256(data).hexdigest(),
    'versions':sorted({r['version'] for r in rows if 'version' in r}),
    'refusals':events('refused'),'configuration':events('bridge_configuration'),
    'proxy_identity':events('ngx_proxy_identity'),'plans':events('bridge_plan'),
    'scale_events':[r for r in rows if r.get('event') in {'baseline','target_requested','target_active','restore_pending','restored_active','sequence_complete'}],
    'bridge_counts':events('bridge_order_summary'),'quad_counts':events('quad_summary'),
    'correlated_groups':[{'phase':phase,'source_rect':list(rect),'texture_size':list(texture),'dispatch':list(dispatch),'records':n}
        for (phase,rect,texture,dispatch),n in sorted(groups.items())],
    'target_pair_records':len(target),'target_matching_invocation_and_actual_rect_records':len(matched),
    'target_nonmatching_pair_records':len(target)-len(matched),
    'cpu_scene_association_observed':bool(matched),'completed_and_restored':restored,
    'stopped':stopped,'in_game_sr_executed':False,'fps_measured':False,
    'limits':['Same CPU invocation and rectangle do not prove GPU resource identity or temporal correctness.',
              'No NGX feature creation/evaluation or copy-rectangle modification occurs in this preflight build.'],
}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(summary,ensure_ascii=False,indent=2),'utf-8')
print(json.dumps({k:summary[k] for k in ['versions','refusals','target_pair_records','target_matching_invocation_and_actual_rect_records',
    'target_nonmatching_pair_records','completed_and_restored','cpu_scene_association_observed']},ensure_ascii=False,indent=2))
