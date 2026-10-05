"""Summarize same-frame, observed MHW texture bindings and copies; never touches the game."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('capture', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if args.capture.resolve() == args.output.resolve():
    parser.error('Output must not replace the capture.')
data = args.capture.read_bytes()
rows = [json.loads(line) for line in data.decode('utf-8-sig').splitlines() if line.strip()]

# Roles come from the locally captured shader reflection/instructions, not PSO addresses.
roles = {
    '2d7b26c742c27db2c83546c6cf6a1dfda47d7cfd4336e69f8c2302de5802b0d1': 'native_taa',
    '3eed9b57bbbdf38d14d9a94596fc93a107aa99eab275af7118b95febbbc1ba18': 'motion_blur_reconstruction',
    '174a09e5b23c7e01fb6c47ce86b61250d695113109abb4ffae7219117c31a8cb': 'tone_mapping',
    'be6127ba48008790fd0e93558b8248cb8acd7c0577899bf50d28e5618ae37b79': 'system_copy',
    'ee05306030b514dd110ff699a8a1a069b1765f2cb15baecb3639dc762c850f05': 'previous_gbuffer_id',
}
pso_roles = {r['pso']: roles[r['sha256']] for r in rows
             if r.get('event') == 'texture_shader_saved' and r.get('sha256') in roles}
by_frame = defaultdict(list)
for row in rows:
    if row.get('detailed_frame') and row.get('event') in {'texture_binding_snapshot', 'post_taa_texture_copy'}:
        by_frame[row['taa_serial']].append(row)

def binding(row, kind, slot):
    found = [b for b in row.get('bindings', []) if b['kind'] == kind and b['slot'] == slot]
    return found[0] if len(found) == 1 else None

frames = []
motion_constants = []
for serial, operations in sorted(by_frame.items()):
    taa = next((r for r in operations if r.get('at') == 'taa'), None)
    if not taa:
        continue
    depth = binding(taa, 'srv', 2)
    output = binding(taa, 'uav', 0)
    color = binding(taa, 'srv', 0)
    current = output['resource'] if output else None
    chain = []
    notes = []
    if not color or not output:
        notes.append('TAA color input or output is unresolved.')
    if pso_roles.get(taa.get('pso')) != 'native_taa':
        notes.append('Native TAA shader role is not confirmed by a captured hash.')
    if not taa.get('requires_same_pipeline_binding_copy'):
        notes.append('Capture lacks current-pipeline descriptor provenance.')
    for operation in operations:
        if operation.get('event') == 'post_taa_texture_copy':
            source = operation['source']['resource']
            destination = operation['destination']['resource']
            if current and source == current:
                chain.append({'kind': 'copy', 'source': source, 'destination': destination,
                              'source_box': operation.get('source_box'), 'after_draw': operation['after_draw']})
                current = destination
            continue
        role = pso_roles.get(operation.get('pso'), 'unknown')
        if role not in {'motion_blur_reconstruction', 'tone_mapping', 'system_copy'}:
            continue
        source = binding(operation, 'srv', 0)
        targets = [t for t in operation.get('targets', []) if t['slot'] == 0 and t['resource'] != '0x0']
        if not source or len(targets) != 1:
            notes.append(f'{role}: source or target is unresolved.')
            continue
        if source['resource'] != current:
            notes.append(f'{role}: input does not match the last observed color output.')
        chain.append({'kind': role, 'source': source['resource'], 'destination': targets[0]['resource'],
                      'draw_index': operation['draw_index'], 'viewport': operation['viewport'],
                      'scissor': operation.get('scissor')})
        current = targets[0]['resource']
        if role == 'motion_blur_reconstruction' and operation.get('graphics_cbv5_copied'):
            words = operation['graphics_cbv5_raw_words']
            values = struct.unpack('<16f', struct.pack('<16I', *words))
            motion_constants.append({'taa_serial': serial, 'samples': words[2], 'shutter_speed': values[4],
                                     'fur_shutter_speed': values[5], 'blur_threshold': values[6]})
    if not any(step['kind'] == 'tone_mapping' for step in chain):
        notes.append('No corresponding tone-mapping output was recorded.')
    if taa['input'] != taa['output'] and not any(step['kind'] == 'system_copy' for step in chain):
        notes.append('Low-resolution frame lacks the final spatial-copy step.')
    frames.append({'taa_serial': serial, 'input': taa['input'], 'output': taa['output'],
                   'taa_color_input': color, 'taa_output': output, 'taa_depth_slot': depth,
                   'taa_depth_slot_is_output_sized': bool(depth and depth['size'] == taa['output']),
                   'color_chain': chain, 'association_notes': notes})

summary = {
    'source': str(args.capture), 'sha256': hashlib.sha256(data).hexdigest(),
    'shader_roles': pso_roles, 'detailed_frames': frames, 'motion_blur_constants': motion_constants,
    'unresolved_bindings': [r for r in rows if r.get('event') == 'unresolved_texture_binding'],
    'quad_vertices': [r for r in rows if r.get('event') == 'post_taa_quad_vertices'],
    'counts': [r for r in rows if r.get('event') == 'bridge_order_summary'],
    'depth_preparations': [r for r in rows if r.get('event') == 'mhwss_depth_preparation'],
    'taa_prepared_depth': [r for r in rows if r.get('event') == 'taa_prepared_depth'],
    'restoration': [r for r in rows if r.get('event') == 'stopped'],
    'limitations': [
        'This traces recorded bindings and copies, not fence-confirmed pixel contents or DLSS execution.',
        'Shader roles require the exact captured hash; unknown variants are left unknown.',
        'TAA depth slot naming does not establish a valid SR depth input.',
        'The in-game MotionBlur option can leave a reconstruction draw and a separate fur shutter parameter.',
    ],
    'in_game_sr_executed': False,
}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2), 'utf-8')
print(json.dumps({'detailed_frames': len(frames), 'frames_with_association_notes': sum(bool(f['association_notes']) for f in frames),
                  'shader_roles': sorted(set(pso_roles.values())), 'motion_blur_constant_samples': motion_constants}, indent=2))
