#!/bin/sh

VERSION_FILE="scmversion.cpp"
BRANCH=$(git rev-parse --abbrev-ref HEAD | tr -d '\r\n')
TAG=$(git describe --tags --dirty | tr -d '\r\n')

SIGNATURE_LINE="// ${BRANCH} ${TAG}"

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
const char* g_scm_branch_str = "${BRANCH}";
const char* g_scm_tag_str = "${TAG}";

EOF

