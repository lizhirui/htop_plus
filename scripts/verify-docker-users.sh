#!/bin/sh
# Read-only check of the Palworld process in the actual curses USER column.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
mkdir -p tests/build
pid=$(docker top palworld-server-lxh -eo pid,comm | awk '$2 == "PalServer-Linux" { print $1; exit }')
case "$pid" in
   ''|*[!0-9]*) echo 'Palworld PID not found' >&2; exit 1 ;;
esac
socket="$project_dir/tests/build/docker-ui-verify.sock"
cleanup() { tmux -S "$socket" kill-server 2>/dev/null || :; }
trap cleanup EXIT INT TERM
tmux -S "$socket" new-session -d -s verify -x 180 -y 45 "env HTOP_POWER_CABLE_PORT=off ./htop_plus --readonly --pid=$pid"
sleep 3
tmux -S "$socket" capture-pane -p -t verify:0.0 > tests/build/ui-docker-users.txt
python3 - <<'PY'
from pathlib import Path
screen = Path('tests/build/ui-docker-users.txt').read_text()
rows = [row for row in screen.splitlines() if '[palworld-server-lxh]steam' in row]
if not rows:
    raise SystemExit('Expected Docker username is missing or truncated')
if not any('PalServer' in row for row in rows):
    raise SystemExit('Docker username was not rendered on the Palworld row')
for row in rows:
    print(row)
PY
tmux -S "$socket" send-keys -t verify:0.0 q
