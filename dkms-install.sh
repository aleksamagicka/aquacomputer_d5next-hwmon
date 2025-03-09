#!/bin/bash

if [[ $EUID -ne 0 ]]; then
  echo "You must run this with superuser priviliges.  Try \"sudo ./dkms-install.sh\"" 2>&1
  exit 1
else
  echo "About to run dkms install steps..."
fi

DRV_NAME="$(basename "$(pwd)" | sed 's/-hwmon$//')"
echo $DRV_NAME

#default is branch name plus dkms modifier
DRV_VERSION="$(git rev-parse --abbrev-ref HEAD)-dkms.$(date -u -d "$(git show -s --format=%ci HEAD)" +%Y%m%d)"
#use below for tagged versioning. Should we add tagged version tracking?
#DRV_VERSION=$(git describe --long).$(date -u -d "$(git show -s --format=%ci HEAD)" +%Y%m%d)
echo $DRV_VERSION
DKMS_DIR=/usr/src/${DRV_NAME}-${DRV_VERSION}
echo $DKMS_DIR

make -f Makefile DRIVER=$DRV_NAME DRIVER_VERSION=$DRV_VERSION DKMS_ROOT_PATH=$DKMS_DIR dkms

echo "Finished running dkms install steps."

exit $RESULT
