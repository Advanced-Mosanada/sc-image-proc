#!/usr/bin/env bash
# Run scipd on the board in the background, so host GUIs can connect to it over
# the network (AIPC on TCP 5600, discovery on UDP 5601). It keeps running after
# this script and the SSH session end; it does not survive a reboot.
#
#   scripts/board-scipd.sh start [scipd options]   # e.g. start --name lab-board
#   scripts/board-scipd.sh stop
#   scripts/board-scipd.sh restart [scipd options]
#   scripts/board-scipd.sh status
#   scripts/board-scipd.sh log                     # follow its output (Ctrl+C to leave)
#
#   SCIP_TARGET=rk3588 scripts/board-scipd.sh start   # the ROCK 5B+
#
# Deploy first (scripts/deploy.sh). The log is /tmp/scipd.log on the board.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/board-env.sh"

cmd="${1:-}"
shift || true
log=/tmp/scipd.log

stop() {
    "${SCIP_SSH[@]}" "if pkill -x scipd; then
        for i in \$(seq 50); do pgrep -x scipd >/dev/null || break; sleep 0.1; done
        pgrep -x scipd >/dev/null && { echo 'scipd did not stop' >&2; exit 1; }
        echo 'scipd stopped'
    else
        echo 'scipd was not running'
    fi"
}

start() {
    "${SCIP_SSH[@]}" "set -e
        bin='$SCIP_BOARD_DIR/bin/scipd'
        [ -x \"\$bin\" ] || { echo \"\$bin not found: run scripts/deploy.sh first\" >&2; exit 1; }
        if pgrep -x scipd >/dev/null; then
            echo \"scipd is already running (pid \$(pgrep -x scipd)); use restart\" >&2; exit 1
        fi
        cd /tmp
        # setsid + nohup: detached from this SSH session, keeps running after it.
        run=nohup; command -v setsid >/dev/null && run='setsid nohup'
        PATH='$SCIP_BOARD_DIR/bin':\$PATH \$run \"\$bin\" $(scip_quote "$@") >'$log' 2>&1 </dev/null &
        sleep 1
        pgrep -x scipd >/dev/null || { echo 'scipd exited:' >&2; cat '$log' >&2; exit 1; }
        cat '$log'"
    echo "connect to ${SCIP_BOARD#*@}:5600 (the GUI finds it by itself on the same network)"
}

case "$cmd" in
    start) start "$@" ;;
    stop) stop ;;
    restart) stop; start "$@" ;;
    status)
        "${SCIP_SSH[@]}" "if pgrep -x scipd >/dev/null; then
            echo \"scipd running (pid \$(pgrep -x scipd)) on $SCIP_BOARD\"; tail -n 5 '$log' 2>/dev/null
        else
            echo 'scipd is not running'; exit 3
        fi" ;;
    log) "${SCIP_SSH_TTY[@]}" "tail -n 50 -f '$log'" ;;
    *) echo "usage: $0 start|stop|restart|status|log [scipd options]" >&2; exit 2 ;;
esac
