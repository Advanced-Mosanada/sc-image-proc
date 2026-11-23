#!/usr/bin/env bash
# Run a deployed program on the board, with the deployment's bin/ first on PATH.
# From a terminal, Ctrl+C stops the program on the board.
#
#   scripts/board-run.sh scip-cli -i pattern:1920x1080 -p gray,blur:5 -n 200
#   scripts/board-run.sh scip-cli -i /home/root/videos/clip.mp4 -p gray -o display
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/board-env.sh"

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

"${SCIP_SSH_TTY[@]}" "cd /tmp && PATH='$SCIP_BOARD_DIR/bin':\$PATH $(scip_quote "$@")"
