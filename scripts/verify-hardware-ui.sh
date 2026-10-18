#!/bin/sh
# Capture actual curses rendering at common terminal sizes using a private tmux.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
mkdir -p tests/build
socket="$project_dir/tests/build/ui-verify.sock"
cleanup() { tmux -S "$socket" kill-server 2>/dev/null || :; }
trap cleanup EXIT INT TERM
tmux -S "$socket" new-session -d -s verify -x 180 -y 45 './htop_plus --readonly'
sleep 7
tmux -S "$socket" capture-pane -p -t verify:0.0 > tests/build/ui-180.txt
tmux -S "$socket" resize-window -t verify:0 -x 120 -y 32
sleep 1
tmux -S "$socket" capture-pane -p -t verify:0.0 > tests/build/ui-120.txt
tmux -S "$socket" resize-window -t verify:0 -x 80 -y 32
sleep 1
tmux -S "$socket" capture-pane -p -t verify:0.0 > tests/build/ui-80.txt
# Fail if labels disappear, the model gets truncated, or values never arrive.
python3 - <<'PY'
from pathlib import Path
import re
for width in (180, 120, 80):
    screen = Path(f'tests/build/ui-{width}.txt').read_text()
    for label in ('RTX 5090', 'GPU[', 'VRM[', 'Tmp[', 'Fan[', 'rpm', 'Pwr[',
                  'A  [', 'F  [', 'Con[', 'Ctl[', 'V / A / W', 'PID'):
        if label not in screen:
            raise SystemExit(f'{width} columns: missing {label!r}')
    if 'N/A' in screen:
        raise SystemExit(f'{width} columns: hardware sample missing')
    cpu_rows = '\n'.join(screen.splitlines()[:13])
    cores = {int(n) for n in re.findall(r'(?<!\S)(\d+)[\[\]]', cpu_rows)}
    if cores != set(range(96)):
        raise SystemExit(f'{width} columns: some CPU IDs are missing')
    if width == 180 and '%]' not in cpu_rows:
        raise SystemExit('180 columns: CPU usage percentages are missing')
PY
tmux -S "$socket" send-keys -t verify:0.0 q
sleep 1
echo 'Captured tests/build/ui-{180,120,80}.txt'
head -n 14 tests/build/ui-180.txt
