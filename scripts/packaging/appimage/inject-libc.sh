#!/usr/bin/env bash

set -e

function retry_command {
  # Package servers tend to be unreliable at times..
  # Retry a bunch of times.
  local RETRIES=10

  for i in $(seq 1 "$RETRIES"); do
    "$@" && break
    if [ "$i" == "$RETRIES" ]; then
      echo "Command \"$@\" failed after ${RETRIES} retries."
      exit 1
    fi
  done
}

this_dir="$(readlink -f "$(dirname "$0")")"

if [ "$#" -ne 5 ]; then
	echo "Syntax: $0 <path to AppDir> <.deb arch> <triple> <ubuntu mirror> <binary to run>"
	echo "e.g. $0 DuckStation.AppDir amd64 x86_64-linux-gnu https://archive.ubuntu.com/ubuntu/ duckstation-qt"
	exit 1
fi


APPDIR=$1
DEBARCH=$2
TRIPLE=$3
MIRROR=$4
APPNAME=$5

LIBC_PACKAGE_URL="${MIRROR}/pool/main/g/glibc/libc6_2.35-0ubuntu3.8_${DEBARCH}.deb"
LIBGCCS_PACKAGE_URL="${MIRROR}/pool/main/g/gcc-12/libgcc-s1_12.3.0-1ubuntu1~22.04_${DEBARCH}.deb"
LIBSTDCXX_PACKAGE_URL="${MIRROR}/pool/main/g/gcc-12/libstdc++6_12.3.0-1ubuntu1~22.04_${DEBARCH}.deb"
GLIBC_VERSION=2.35

mkdir "temp"
cd "temp"
retry_command wget -O "libc.deb" "${LIBC_PACKAGE_URL}"
retry_command wget -O "libgccs.deb" "${LIBGCCS_PACKAGE_URL}"
retry_command wget -O "libstdc++.deb" "${LIBSTDCXX_PACKAGE_URL}"
dpkg -x "libc.deb" .
dpkg -x "libgccs.deb" .
dpkg -x "libstdc++.deb" .

# Copy everything into AppDir
RUNTIME="${APPDIR}/libc-runtime"
mkdir -p "${RUNTIME}"

# libc.so.6 and friends
cd "lib/${TRIPLE}"
cp -v * "${RUNTIME}"
cd ../../

# libstdc++
cd "usr/lib/${TRIPLE}"
cp -v * "${RUNTIME}" || true
cd ../../..

# done with temps now
cd ..
rm -fr temp

# Not risking mixing resolvers...
cd "${RUNTIME}"
rm -vf libnss_*

# Move ld-linux.so.2 into the binary directory so we can preserve arg0's directory
mv -v "ld-linux-"*.so.2 "${APPDIR}/usr/bin/ld-linux"

# Set up the replacement apprun script
cd "${APPDIR}"
rm -f AppRun.wrapped
cp "${this_dir}/inject-libc-apprun.sh" AppRun.wrapped
sed -i -e "s/__APPNAME__/${APPNAME}/" AppRun.wrapped
sed -i -e "s/__REQ_GLIBC_VERSION__/${GLIBC_VERSION}/" AppRun.wrapped

echo Done.

