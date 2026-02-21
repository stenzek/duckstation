#!/usr/bin/env bash

REQUIRED_GLIBC_VERSION="__REQ_GLIBC_VERSION__"

this_dir="$(readlink -f "$(dirname "$0")")"

APPBIN="${this_dir}/usr/bin/__APPNAME__"
RUNTIME_DIR="${this_dir}/libc-runtime"
LOADER_BIN="${this_dir}/usr/bin/ld-linux"

GLIBC_VERSION=$(ldd --version | head -1 | sed -e 's/.* \([0-9.]\)/\1/')

echo "Detected glibc version ${GLIBC_VERSION}."

if [[ -z "${GLIBC_VERSION}" || ! "${GLIBC_VERSION}" < "${REQUIRED_GLIBC_VERSION}" ]]; then
	echo "Using system libc/libstdc++."
	exec "${APPBIN}" "$@"
fi


echo "Using bundled libc/libstdc++ from ${RUNTIME_DIR}."
if [ -z "$LD_LIBRARY_PATH" ]; then
	export LD_LIBRARY_PATH="${RUNTIME_DIR}"
else
	export LD_LIBRARY_PATH="${RUNTIME_DIR}:${LD_LIBRARY_PATH}"
fi

exec "${LOADER_BIN}" "${APPBIN}" "$@"

