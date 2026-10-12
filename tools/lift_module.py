"""Generate a whole-module translation audit. This does not make a playable build."""
import argparse
import collections
import json
import sys
import subprocess
import hashlib
from concurrent.futures import ThreadPoolExecutor
from inputs import ROOT, PROFILE, verify_module
sys.path.insert(0, str(ROOT / "vendor/lifter"))
from lift import Lifter, find_cycles


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("module", choices=list(PROFILE["modules"]))
    ap.add_argument("analysis")
    ap.add_argument("--check-c", action="store_true", help="Syntax-check all generated C with Clang")
    ap.add_argument("--prepared", action="store_true", help="Translate the verified relocated image")
    ap.add_argument("--imp-base", type=int, default=0)
    args = ap.parse_args()
    binary = verify_module(args.module)
    if args.prepared:
        layout = json.loads((ROOT / "work/images/layout.json").read_text())[args.module]
        binary = ROOT / "work/images" / args.module
        if not layout["prepared"] or hashlib.sha256(binary.read_bytes()).hexdigest() != layout["sha256"]:
            raise RuntimeError("Prepared image missing or changed; rerun relocation")
    analysis = ROOT / args.analysis
    lifter = Lifter(str(binary), str(analysis))
    lifter.hooks = {}
    lifter.overrides = {}
    lifter.illegal_seh = False  # Keep unimplemented instructions as explicit recompiler faults.
    lifter.jt_cache = {}
    lifter.imp_index = {va: args.imp_base + k for k, (va, _) in enumerate(lifter.imports)}
    queue = collections.deque(sorted(lifter.funcs))
    done = {}
    while queue:
        address = queue.popleft()
        if address in done:
            continue
        source, calls = lifter.lift_function(address)
        done[address] = source
        for callee in sorted(calls):
            if callee not in lifter.funcs:
                lifter.funcs.add(callee)
                lifter.discovered.append(callee)
                queue.append(callee)
    for group, members in enumerate(find_cycles(lifter)):
        for address in members:
            lifter.scc_of[address] = group
    for address in lifter.scc_of:
        done[address] = lifter.lift_function(address)[0]
    out = ROOT / "work/lifted" / args.module
    out.mkdir(parents=True, exist_ok=True)
    (out / "funcs.h").write_text('#include "rt.h"\n' + ''.join(
        f"void f_{address:08x}(Ctx *);\n" for address in sorted(done)))
    for old in out.glob("lift_*.c"):
        old.unlink()
    addresses = sorted(done)
    for offset in range(0, len(addresses), 100):
        (out / f"lift_{offset//100:03d}.c").write_text('#include "funcs.h"\n' + '\n'.join(
            done[address] for address in addresses[offset:offset+100]))
    syntax_errors = []
    if args.check_c:
        def check(path):
            result = subprocess.run(["clang", "-arch", "arm64", "-fsyntax-only", "-Wno-unused-value",
                                     "-I", str(ROOT / "runtime"), str(path)], capture_output=True, text=True)
            return None if result.returncode == 0 else dict(file=path.name, diagnostics=result.stderr)
        with ThreadPoolExecutor(max_workers=4) as pool:
            syntax_errors = [error for error in pool.map(check, sorted(out.glob("lift_*.c"))) if error]
    report = dict(module=args.module, functions=len(done), discovered=len(lifter.discovered),
                  unsupported=dict(lifter.unsup), internal_errors=lifter.internal,
                  unresolved_jump_tables=sorted(set(lifter.unres_jt)),
                  status="Translation audit only; generated functions may contain explicit traps",
                  syntax_checked=args.check_c, syntax_errors=syntax_errors,
                  compiled=False, gameplay_tested=False)
    (out / "audit.json").write_text(json.dumps(report, indent=2) + "\n")
    (out / "image.json").write_text(json.dumps(dict(
        module=args.module, base=lifter.pe.base, entry=lifter.pe.entry,
        sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        binary=str(binary.relative_to(ROOT)), imp_base=args.imp_base,
        functions=addresses, imports=lifter.imports), indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("module", "functions", "unsupported", "internal_errors")}, indent=2))
    if args.check_c:
        print(f"C syntax check: {len(syntax_errors)} failed translation units")
    if syntax_errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
