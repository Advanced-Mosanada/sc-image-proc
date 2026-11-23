#!/usr/bin/env bash
# Run the unit tests on the board (deploy first). Extra arguments go to GoogleTest:
#
#   scripts/board-test.sh
#   scripts/board-test.sh --gtest_filter='ToGray.*'
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/board-env.sh"

"${SCIP_SSH[@]}" "cd /tmp && '$SCIP_BOARD_DIR/libexec/scip/scip_tests' $(scip_quote "$@")"
