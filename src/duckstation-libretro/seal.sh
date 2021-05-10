#!/usr/bin/env bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <key>"
  exit 1
fi

SCRIPTDIR=$(dirname $(realpath "${BASH_SOURCE[0]}"))

IFS="
"

ls $SCRIPTDIR/*.cpp $SCRIPTDIR/*.h $SCRIPTDIR/*.py 1>/dev/null 2>/dev/null
if [ $? -ne 0 ]; then
  echo "No unsealed files"
  exit 1
fi

for i in $SCRIPTDIR/*.cpp $SCRIPTDIR/*.h $SCRIPTDIR/*.py; do
  NEWNAME="${i}.gpg"

  if [ -f $NEWNAME ]; then
    OLDHASH=$(gpg -d --batch --passphrase "$1" $NEWNAME 2>/dev/null | md5sum | cut -d' ' -f1)
    NEWHASH=$(md5sum $i | cut -d' ' -f1)

    if [ "$OLDHASH" = "$NEWHASH" ]; then
      echo "** No changes in ${i}, skipping update."
      continue
    fi
  fi

  echo "** Updating ${i} -> ${NEWNAME}"
  rm -f "${i}.gpg"
  gpg --batch --passphrase "$1" -c $i
done
