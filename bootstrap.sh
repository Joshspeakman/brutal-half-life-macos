#!/bin/bash
set -euo pipefail
BHL_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$BHL_ROOT"
if [[ $# != 1 ]]; then
    echo 'Usage: ./bootstrap.sh /path/to/bhl_v3.zip (or the game-files folder install.sh keeps)' >&2
    exit 2
fi
xcode-select -p >/dev/null 2>&1 || { echo 'Install Xcode Command Line Tools first.' >&2; exit 1; }
[[ "$(uname -m)" == arm64 ]] || { echo 'Run this with an arm64 shell on Apple Silicon.' >&2; exit 1; }
if [[ ! -x .venv/bin/python ]]; then python3 -m venv .venv; fi
if ! .venv/bin/python -c 'import capstone, pefile, unicorn; assert (capstone.__version__, pefile.__version__, unicorn.__version__) == ("5.0.7", "2024.8.26", "2.1.4")' >/dev/null 2>&1; then
    .venv/bin/python -m pip install -r requirements.txt
fi
.venv/bin/python tools/prepare.py "$1"
.venv/bin/python tools/survey.py
.venv/bin/python tools/relocate.py
.venv/bin/python tools/build_probe.py
.venv/bin/python tests/test_interfaces.py
echo 'Native interface milestone passed. This is not a playable game build.'
