"""Execute the actual recompiled CRT and DLL_PROCESS_ATTACH entry point."""
import argparse
import json
from native_harness import Native, ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--module", default="hl.dll")
    args = ap.parse_args()
    n = Native(trace=args.trace)
    result = n.attach(args.module)
    assert result == 1, f"DllMain rejected attach: {result}"
    report = dict(module=args.module, architecture="arm64", dll_process_attach=result,
                  heap_bytes=n.lib.bhl_heap_used(), gameplay_tested=False)
    (ROOT / "work/reports" / (args.module + "-startup.json")).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
