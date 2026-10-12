"""Pinned local inputs; original files and generated code stay under work/."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
PROFILE = json.loads((ROOT / "config/modules.json").read_text())


def verify_module(name):
    path = ROOT / "work/inputs" / name
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    expected = PROFILE["modules"][name]["sha256"]
    if actual != expected:
        raise ValueError(f"{name}: expected SHA256 {expected}, got {actual}")
    return path
