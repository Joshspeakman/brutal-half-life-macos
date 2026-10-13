#!/bin/bash
set -euo pipefail
BHL_PROJECT="$(cd "$(dirname "$0")" && pwd)"
BHL_ARCHIVE=''
BHL_VALVE=''
BHL_APP="$HOME/Applications/Brutal Half-Life.app"
BHL_DATA="$HOME/Library/Application Support/Brutal Half-Life"
BHL_CHECK=()
usage() {
    echo 'Usage: ./install.sh --archive /path/to/bhl_v3.zip --valve /path/to/Half-Life/valve [--app /path/to/Brutal Half-Life.app] [--data-dir /path/to/data] [--check]'
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --archive|--valve|--app|--data-dir)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            case "$1" in
                --archive) BHL_ARCHIVE="$2";;
                --valve) BHL_VALVE="$2";;
                --app) BHL_APP="$2";;
                --data-dir) BHL_DATA="$2";;
            esac
            shift 2;;
        --check) BHL_CHECK=(--check --check-client); shift;;
        --help|-h) usage; exit 0;;
        *) usage >&2; exit 2;;
    esac
done
[[ -f "$BHL_ARCHIVE" && -d "$BHL_VALVE" ]] || { usage >&2; exit 2; }
# Resolve user inputs before changing directory; preserve spaces throughout.
BHL_ARCHIVE="$(cd "$(dirname "$BHL_ARCHIVE")" && pwd)/$(basename "$BHL_ARCHIVE")"
BHL_VALVE="$(cd "$BHL_VALVE" && pwd)"
[[ "$BHL_APP" = /* ]] || BHL_APP="$PWD/$BHL_APP"
[[ "$BHL_DATA" = /* ]] || BHL_DATA="$PWD/$BHL_DATA"
cd "$BHL_PROJECT"
./bootstrap.sh "$BHL_ARCHIVE"
.venv/bin/python tools/build_project.py --archive "$BHL_ARCHIVE" --valve "$BHL_VALVE" ${BHL_CHECK[@]+"${BHL_CHECK[@]}"}
.venv/bin/python tools/package_app.py --archive "$BHL_ARCHIVE" --valve "$BHL_VALVE" --app "$BHL_APP" --data-dir "$BHL_DATA" --replace
echo 'Local app built. Open it in Finder to play; no nightly build was published.'
