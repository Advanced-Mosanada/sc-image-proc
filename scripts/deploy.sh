#!/usr/bin/env bash
# Build a board preset, install it to install/<preset>, and copy it to the board.
#
#   scripts/deploy.sh                      # imx8mp-release
#   scripts/deploy.sh imx8mp-relwithdebinfo
#
# The board has no rsync, so the install tree is streamed over ssh as a tar.
# The previous deployment is replaced atomically-ish (unpack to .new, then swap).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/board-env.sh"

preset="${1:-imx8mp-release}"
case "$preset" in
    imx8mp-*) ;;
    *) echo "deploy.sh: '$preset' is not a board preset (imx8mp-*)" >&2; exit 2 ;;
esac

build_dir="$SCIP_ROOT/build/$preset"
stage_dir="$SCIP_ROOT/install/$preset"

if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    cmake --preset "$preset"
fi
cmake --build --preset "$preset"

# Release builds are stripped (smaller, faster to copy); debug ones keep symbols.
strip=()
[[ "$preset" == "imx8mp-release" ]] && strip=(--strip)
rm -rf "${stage_dir:?}"
cmake --install "$build_dir" --prefix "$stage_dir" "${strip[@]}"

echo "deploying $preset -> $SCIP_BOARD:$SCIP_BOARD_DIR"
tar -C "$stage_dir" -cf - . | "${SCIP_SSH[@]}" "set -e
    rm -rf '$SCIP_BOARD_DIR.new'
    mkdir -p '$SCIP_BOARD_DIR.new'
    tar -C '$SCIP_BOARD_DIR.new' -m -xf -   # -m: board clock may lag the PC
    # A running scipd keeps its binary busy (NFS cannot remove it): stop it.
    if pkill -x scipd; then
        echo 'stopping the running scipd'
        for i in \$(seq 50); do pgrep -x scipd >/dev/null || break; sleep 0.1; done
    fi
    rm -rf '$SCIP_BOARD_DIR'
    mv '$SCIP_BOARD_DIR.new' '$SCIP_BOARD_DIR'"

"${SCIP_SSH[@]}" "'$SCIP_BOARD_DIR/bin/scip-cli' --version"
