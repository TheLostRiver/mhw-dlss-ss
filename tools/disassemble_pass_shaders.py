"""Disassemble saved shader containers once per SHA256; preserve a mapping to each observed PSO file."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
from extract_shaders import disassemble

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("folder", type=Path)
parser.add_argument("--dxc", type=Path, default=shutil.which("dxc"), help="DXC executable for DXIL containers")
args = parser.parse_args()
folder = args.folder
output = folder.parent / "disassembly"
output.mkdir(exist_ok=True)
unique = {}
entries = []
for path in sorted(folder.glob("*.dxbc")):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest not in unique:
        hr, text = disassemble(data)
        kind = "dxbc"
        if text is None and args.dxc:
            run = subprocess.run([str(args.dxc), "-dumpbin", str(path)],
                                 capture_output=True, text=True, timeout=30)
            if run.returncode == 0:
                text, kind = run.stdout, "dxil"
        if text is not None:
            (output / (digest + ".asm")).write_text(text, encoding="utf-8")
        unique[digest] = {"bytes": len(data), "disassembled": text is not None, "kind": kind,
                          "disassembly": digest + ".asm" if text is not None else None}
    entries.append({"file": path.name, "sha256": digest, **unique[digest]})
manifest = {"entries": entries, "files": len(entries), "unique_containers": len(unique),
            "unique_disassembled": sum(entry["disassembled"] for entry in unique.values())}
(folder.parent / "shader-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print(json.dumps({key: value for key, value in manifest.items() if key != "entries"}, indent=2))
