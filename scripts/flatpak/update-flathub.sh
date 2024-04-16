#!/usr/bin/env bash

APPID=org.duckstation.DuckStation
SCRIPTDIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))

if [[ $# -lt 1 ]]; then
	echo "Output directory must be provided as a parameter"
	exit 1
fi

OUTDIR=$(realpath "$1")
OUTMANIFEST="${OUTDIR}/${APPID}.json"

echo -n "Get revision: "
pushd "${SCRIPTDIR}" >/dev/null
GIT_HASH=$(git rev-parse HEAD)
popd >/dev/null
echo "${GIT_HASH}"

echo "Updating files in ${OUTDIR}..."
mkdir -p "${OUTDIR}"
rm -fr "${OUTDIR}/modules"
cp -a "${SCRIPTDIR}/modules" "${OUTDIR}/modules"
cp "${SCRIPTDIR}/../shaderc-changes.patch" "${OUTDIR}/modules"

echo "Generate AppStream XML..."
"${SCRIPTDIR}/../../scripts/generate-metainfo.sh" "${OUTDIR}"

echo "Patching Manifest Sources..."
jq ".sources[4] = {\"type\": \"patch\", \"path\": \"shaderc-changes.patch\"}" \
"${SCRIPTDIR}/modules/22-shaderc.json" > "${OUTDIR}/modules/22-shaderc.json"

jq ".modules[3].sources = ["\
"{\"type\": \"git\", \"url\": \"https://github.com/stenzek/duckstation.git\", \"commit\": \"${GIT_HASH}\", \"disable-shallow-clone\": true},"\
"{\"type\": \"file\", \"path\": \"org.duckstation.DuckStation.metainfo.xml\", \"dest\": \"scripts/flatpak\"}]" \
"${SCRIPTDIR}/${APPID}.json" > "${OUTMANIFEST}"
