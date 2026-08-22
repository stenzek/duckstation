#!/usr/bin/env bash

set -e

if (( $# != 1 )); then
	echo "Usage: $0 translation.ts" >&2
	exit 1
fi

filename=$1
if [[ ! -f "$filename" || "$filename" != *.ts ]]; then
	echo "Error: '$filename' is not an existing .ts file." >&2
	exit 1
fi
filename=$(realpath "$filename")

scriptdir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
linguist="$scriptdir/../../../dep/prebuilt/linux-x64/bin"
context=".././ ../../core/ ../../util/ -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATE,QT_TRANSLATE_NOOP+=TRANSLATE_SV,QT_TRANSLATE_NOOP+=TRANSLATE_STR,QT_TRANSLATE_NOOP+=TRANSLATE_FS,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_SV,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_STR,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_FS,QT_TRANSLATE_N_NOOP3+=TRANSLATE_PLURAL_NOOP,QT_TRANSLATE_NOOP+=TRANSLATE_NOOP,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_NOOP,translate+=TRANSLATE_PLURAL_STR,translate+=TRANSLATE_PLURAL_SSTR,translate+=TRANSLATE_PLURAL_FS -no-obsolete -locations none"

echo "Cleaning $(basename "$filename")..."
cd "$scriptdir"
# context is intentionally unquoted so each lupdate option is passed separately.
"$linguist/lupdate" $context -ts "$filename"
echo "$(basename "$filename") cleanup completed, ready for pull request."
