"""Extract only verified modules from the user's original v3 ZIP (or the originals install.sh keeps)."""
import argparse
import hashlib
import json
from inputs import ROOT, PROFILE
from originals import Originals


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("archive")
    args = ap.parse_args()
    # Validate all payloads before writing any of them.
    source = Originals(args.archive)
    payloads = {}
    for name, spec in PROFILE["modules"].items():
        if "member" not in spec:
            continue
        payload = source.read(spec["member"])
        actual = hashlib.sha256(payload).hexdigest()
        if actual != spec["sha256"]:
            raise SystemExit(f"{name}: unsupported input SHA256 {actual}; expected {spec['sha256']}")
        payloads[name] = payload
    out = ROOT / "work/inputs"
    out.mkdir(parents=True, exist_ok=True)
    for name, payload in payloads.items():
        staged = out / (name + ".new")
        staged.write_bytes(payload)
        staged.replace(out / name)
    print(json.dumps({"release": PROFILE["release"], "verified_modules": list(payloads)}))


if __name__ == "__main__":
    main()
