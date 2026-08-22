#!/bin/bash

set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf -- "$TEST_DIR"' EXIT

check_help() {
    local script=$1
    local output

    output=$(cd -- "$TEST_DIR" && "$REPO_ROOT/$script" --help 2>&1)
    grep -q '^Usage:' <<<"$output"
    ! grep -q 'libinstall.sh' <<<"$output"

    output=$(cd -- "$TEST_DIR" && sh "$REPO_ROOT/$script" --help 2>&1)
    grep -q '^Usage:' <<<"$output"
    ! grep -q 'libinstall.sh' <<<"$output"
}

check_help volcinstall.sh
check_help volcuninstall.sh
