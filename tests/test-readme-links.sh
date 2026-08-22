#!/bin/bash

set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
README="$REPO_ROOT/README.md"

check_manual_link() {
    local label=$1
    local target=$2
    local file=$3

    grep -Fq "[$label]($target)" "$README"
    test -f "$REPO_ROOT/$file"
}

check_manual_link 'Installation guide' \
    'docs/volclava%20%E5%AE%89%E8%A3%85%E5%8F%8A%E9%85%8D%E7%BD%AE%E6%96%87%E6%A1%A3.pdf' \
    'docs/volclava 安装及配置文档.pdf'
check_manual_link 'User Guide' \
    'docs/volclava%20%E7%94%A8%E6%88%B7%E6%89%8B%E5%86%8C.pdf' \
    'docs/volclava 用户手册.pdf'
check_manual_link 'Administrator Guide' \
    'docs/volclava%20%E7%AE%A1%E7%90%86%E5%91%98%E6%89%8B%E5%86%8C.pdf' \
    'docs/volclava 管理员手册.pdf'
