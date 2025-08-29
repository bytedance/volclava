#!/bin/sh
set -x
rm -f config.cache
aclocal 
autoconf
autoheader
automake --add-missing --copy
set +x
./configure "$@"
