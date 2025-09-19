#!/bin/sh
set -x

#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (c) 2011-2012 David Bigagli
#

major="2"
minor="1"

GIT_LAST_COMMIT=$(git log -1  --pretty=format:%H)
GIT_LAST_DATE=$(git log -1 --pretty=format:"%ad" --date=short)
GIT_LAST_DATE_FORMAT=$(date -d "${GIT_LAST_DATE}" "+%b %d %Y")
sed -i "s:\" __DATE__\":${GIT_LAST_DATE_FORMAT}:g" lsf/lsf.h
git config --global user.name "temp"
git config --global user.email "temp@commit.com"
git commit -a -m 'temp commit for make rpm with __DATE__'
git show

grep 4.6 /etc/redhat-release > /dev/null
if [ "$?" == "0" ]; then
   echo "Cleaning..."
   rm -f /usr/src/redhat/SOURCES/volclava*
   rm -f /usr/src/redhat/RPMS/x86_64/volclava*
   rm -f /usr/src/redhat/SPECS/volclava.spec

   echo "Archiving..."
   git archive --format=tar --prefix="volclava-${major}.${minor}/" HEAD \
   | gzip > /usr/src/redhat/SOURCES/volclava-${major}.${minor}.tar.gz
   cp spec/volclava.spec /usr/src/redhat/SPECS/volclava.spec

  git reset --hard ${GIT_LAST_COMMIT}
  echo "RPM building..."
  rpmbuild -ba --target x86_64 /usr/src/redhat/SPECS/volclava.spec
  if [ "$?" != 0 ]; then
    echo "Failed buidling rpm"
    exit 1
  fi
  exit 0
fi 
   
echo "Cleaning up ~/rpmbuild directory..."
rm -rf ~/rpmbuild

echo "Creating the ~/rpmbuild..."
rpmdev-setuptree

echo "Archving source code..."
git archive --format=tar --prefix="volclava-${major}.${minor}/" HEAD \
   | gzip > ~/rpmbuild/SOURCES/volclava-${major}.${minor}.tar.gz
cp spec/volclava.spec ~/rpmbuild/SPECS/volclava.spec
git reset --hard ${GIT_LAST_COMMIT}

echo "RPM building..."
rpmbuild -ba --target x86_64 ~/rpmbuild/SPECS/volclava.spec
if [ "$?" != 0 ]; then
  echo "Failed buidling rpm"
  exit 1
fi
