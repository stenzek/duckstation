#!/bin/sh

VERSION_FILE="scmversion.cpp"

CURDIR=$(pwd)
if [ "$(uname -s)" = "Darwin" ]; then
  cd "$(dirname $(python -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$0"))"
else
  cd $(dirname $(readlink -f $0))
fi


HASH=$(git rev-parse HEAD)
BRANCH=$(git rev-parse --abbrev-ref HEAD | tr -d '\r\n')
TAG=$(git describe --tags --dirty --exclude latest --exclude preview --exclude play-store-release | tr -d '\r\n')
DATE=$(git log -1 --date=iso8601-strict --format=%cd)

cd $CURDIR

SIGNATURE_LINE="// ${HASH} ${BRANCH} ${TAG} ${DATE}"

if [ -f $VERSION_FILE ]; then
  EXISTING_LINE=$(head -n1 $VERSION_FILE | tr -d '\n')
  if [ "$EXISTING_LINE" = "$SIGNATURE_LINE" ]; then
    echo "Signature matches, skipping writing ${VERSION_FILE}"
    exit 0
  fi
fi

echo "Writing ${VERSION_FILE}..."

cat > $VERSION_FILE << EOF
${SIGNATURE_LINE}
const char* g_scm_hash_str = "${HASH}";
const char* g_scm_branch_str = "${BRANCH}";
const char* g_scm_tag_str = "${TAG}";
const char* g_scm_date_str = "${DATE}";

EOF

