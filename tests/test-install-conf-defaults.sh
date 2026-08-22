#!/bin/bash

set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

unset VOLC_PREFIX
source "$REPO_ROOT/install.conf.example"

test -z "${VOLC_PREFIX:-}"
grep -Fq '#VOLC_PREFIX=/opt/volclava-2.2' \
    "$REPO_ROOT/install.conf.example"
