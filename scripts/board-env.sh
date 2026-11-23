# Shared settings for the board scripts. Sourced, not executed.
# Override any of these from the environment, e.g.  SCIP_BOARD=root@10.0.0.5 scripts/deploy.sh

SCIP_BOARD="${SCIP_BOARD:-root@192.168.50.2}"
SCIP_BOARD_DIR="${SCIP_BOARD_DIR:-/opt/scip}"
SCIP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCIP_SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$SCIP_BOARD")
# Interactive variant: a remote terminal, so Ctrl+C stops the program on the board.
if [[ -t 0 && -t 1 ]]; then
    SCIP_SSH_TTY=(ssh -t -o BatchMode=yes -o ConnectTimeout=5 "$SCIP_BOARD")
else
    SCIP_SSH_TTY=("${SCIP_SSH[@]}")
fi

case "$SCIP_BOARD_DIR" in
    /opt/?* | /home/?* | /tmp/?*) ;;
    *) echo "refusing SCIP_BOARD_DIR='$SCIP_BOARD_DIR' (must be below /opt, /home or /tmp)" >&2
       exit 2 ;;
esac

# Quote arguments so they survive the remote shell unchanged.
scip_quote() {
    printf '%q ' "$@"
}
