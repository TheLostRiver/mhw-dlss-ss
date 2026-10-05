"""Whitelist only previously inspected hook owners/targets in the pass observer's local INI."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("report", type=Path)
parser.add_argument("ini", type=Path)
args = parser.parse_args()
records = json.loads(args.report.read_text())
config = configparser.ConfigParser()
config.optionxform = str
config.read(args.ini, encoding="utf-8-sig")
section = config["Capture"]
for record in records:
    owner = record["owner"]
    if not owner or owner["name"].lower() != "d3d12.dll":
        raise ValueError("This setup accepts only the inspected OptiScaler D3D12 proxy")
    digest = hashlib.sha256(Path(owner["path"]).read_bytes()).hexdigest()
    if digest != "73cf97e5c1a3db2be778df25d21b2999e664a06c6e0f64e67741987a21228a24":
        raise ValueError("Unrecognized proxy file hash")
    prefix = "Existing" + record["name"]
    section[prefix + "OwnerPath"] = owner["path"]
    section[prefix + "OwnerSha256"] = digest
    section[prefix + "TargetRva"] = record["target_rva"]
    section[prefix + "TargetBytes"] = record["target_bytes"]
section["Enabled"] = "1"
with args.ini.open("w", encoding="utf-16") as output:
    config.write(output, space_around_delimiters=False)
print(f"Configured {len(records)} inspected existing hooks; the DLL will verify their live targets again.")
