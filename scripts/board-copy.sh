#!/usr/bin/env bash
# Copy files (videos, images) to the board.
#
#   scripts/board-copy.sh ~/videos/*.mp4             # -> /home/root/videos/
#   SCIP_BOARD_DATA=/tmp scripts/board-copy.sh x.mp4  # /tmp is RAM on the board: fast, not kept
#
# The board's root filesystem is NFS from the PC, so /home/root/videos lives on the
# PC disk; reading it on the board goes over the network.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/board-env.sh"

dest="${SCIP_BOARD_DATA:-/home/root/videos}"
if [[ $# -eq 0 ]]; then
    echo "usage: $0 <file>... (destination: $dest, override with SCIP_BOARD_DATA)" >&2
    exit 2
fi

tar_args=()
for file in "$@"; do
    [[ -f "$file" ]] || { echo "not a file: $file" >&2; exit 2; }
    tar_args+=(-C "$(cd "$(dirname "$file")" && pwd)" "$(basename "$file")")
done

tar -cf - "${tar_args[@]}" | "${SCIP_SSH[@]}" "mkdir -p '$dest' && tar -C '$dest' -m -xf -"
for file in "$@"; do
    echo "$SCIP_BOARD:$dest/$(basename "$file")"
done
