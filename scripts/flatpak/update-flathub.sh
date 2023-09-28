#!/usr/bin/env bash

APPID=org.duckstation.DuckStation
SCRIPTDIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))

if [[ $# -lt 1 ]]; then
	echo "Output directory must be provided as a parameter"
	exit 1
fi

OUTDIR=$(realpath "$1")

rm -fr "${OUTDIR}/modules"
cp -a "${SCRIPTDIR}/modules" "${OUTDIR}/modules"
cp "${SCRIPTDIR}/flathub.json" "${OUTDIR}/${APPID}.json"

pushd "${SCRIPTDIR}"
GIT_DATE=$(git log -1 --pretty=%cd --date=short)
GIT_VERSION=$(git tag --points-at HEAD)
GIT_HASH=$(git rev-parse HEAD)

if [[ "${GIT_VERSION}" == "" ]]; then
	GIT_VERSION=$(git describe --tags --dirty --exclude latest --exclude preview --exclude legacy --exclude previous-latest | tr -d '\r\n')
	if [[ "${GIT_VERSION}" == "" ]]; then
		GIT_VERSION=$(git rev-parse HEAD)
	fi
fi
"${SCRIPTDIR}/../../scripts/generate-metainfo.sh" "${OUTDIR}/${APPID}.metainfo.xml"
popd

# Change App ID, because flathub uses the wrong name.
sed -i -e "s/org.duckstation.duckstation/org.duckstation.DuckStation/g" "${OUTDIR}/${APPID}.json" "${OUTDIR}/${APPID}.metainfo.xml"

# Fill in version details.
sed -i -e "s/@GIT_VERSION@/${GIT_VERSION}/" "${OUTDIR}/${APPID}.json"
sed -i -e "s/@GIT_DATE@/${GIT_DATE}/" "${OUTDIR}/${APPID}.json"
sed -i -e "s/@GIT_HASH@/${GIT_HASH}/" "${OUTDIR}/${APPID}.json"

# Apparently we don't have git history.
pushd "${OUTDIR}"
"${SCRIPTDIR}/../../src/scmversion/gen_scmversion.sh"
popd
