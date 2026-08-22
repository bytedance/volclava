#!/bin/bash

set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BMOD_MAN="$REPO_ROOT/lsbatch/man1/bmod.1"

grep -Fq '[\fB-a \fIapplication_name\fR ...]' "$BMOD_MAN"
grep -Fq '\fB-a \fIapplication_name\fR ...' "$BMOD_MAN"
grep -Fq '\fBLSB_SUB_ADDITIONAL\fR' "$BMOD_MAN"
grep -Fq '\fBLSB_SUB_PARM_FILE\fR' "$BMOD_MAN"
