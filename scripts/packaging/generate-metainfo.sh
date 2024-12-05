#!/usr/bin/env bash

SCRIPTDIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))

if [[ $# -lt 1 ]]; then
	echo "Output directory must be provided as a parameter"
	exit 1
fi

APPID="org.duckstation.DuckStation"
OUTDIR=$(realpath "$1")
OUTFILE="${OUTDIR}/${APPID}.metainfo.xml"

pushd "${SCRIPTDIR}" >/dev/null
GIT_DATE=$(git log -1 --pretty=%cd --date=short)
GIT_HASH=$(git rev-parse HEAD)
GIT_VERSION=$(git describe --dirty | tr -d '\r\n')
if [[ "${GIT_VERSION}" == "" ]]; then
	GIT_VERSION=$(git rev-parse HEAD)
fi

popd >/dev/null

echo "GIT_DATE: ${GIT_DATE}"
echo "GIT_VERSION: ${GIT_VERSION}"
echo "GIT_HASH: ${GIT_HASH}"

cp "${SCRIPTDIR}/${APPID}.metainfo.xml.in" "${OUTFILE}"

sed -i -e "s/@GIT_VERSION@/${GIT_VERSION}/" "${OUTFILE}"
sed -i -e "s/@GIT_DATE@/${GIT_DATE}/" "${OUTFILE}"
sed -i -e "s/@GIT_HASH@/${GIT_HASH}/" "${OUTFILE}"

