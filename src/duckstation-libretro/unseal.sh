#!/usr/bin/env bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <key>"
  exit 1
fi

SCRIPTDIR=$(dirname $(realpath "${BASH_SOURCE[0]}"))

IFS="
"

ls $SCRIPTDIR/*.gpg 1>/dev/null 2>/dev/null
if [ $? -ne 0 ]; then
  echo "No sealed files"
  exit 1
fi

for i in $SCRIPTDIR/*.gpg; do
  ORIGNAME=$(echo $i | sed -e 's/\.gpg$//')
  echo "${i} -> ${ORIGNAME}"
  rm -f $ORIGNAME
  gpg --batch --passphrase "$1" $i
done
