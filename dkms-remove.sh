#!/bin/bash

if [[ $EUID -ne 0 ]]; then
  echo "You must run this with superuser priviliges.  Try \"sudo ./dkms-remove.sh\"" 2>&1
  exit 1
else
  echo "About to run dkms removal steps..."
fi

DRV_NAME="$(basename "$(pwd)" | sed 's/-hwmon$//')"
DRV_VERSION=$(dkms status ${DRV_NAME} -k $(uname -r) | cut -d, -f1 | cut -d/ -f2)

make -f Makefile DRIVER=$DRV_NAME DRIVER_VERSION=$DRV_VERSION dkms_clean

RESULT=$?
if [[ "$RESULT" != "0" ]]; then
  echo "Error occurred while running dkms remove." 2>&1
else
  echo "Finished running dkms removal steps."
fi

exit $RESULT
