#!/usr/bin/env bash

set -e

scriptdir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
linguist="$scriptdir/../../../dep/prebuilt/linux-x64/bin"
context=".././ ../../core/ ../../util/ -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATE,QT_TRANSLATE_NOOP+=TRANSLATE_SV,QT_TRANSLATE_NOOP+=TRANSLATE_STR,QT_TRANSLATE_NOOP+=TRANSLATE_FS,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_SV,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_STR,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_FS,QT_TRANSLATE_N_NOOP3+=TRANSLATE_PLURAL_NOOP,QT_TRANSLATE_NOOP+=TRANSLATE_NOOP,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_NOOP,translate+=TRANSLATE_PLURAL_STR,translate+=TRANSLATE_PLURAL_SSTR,translate+=TRANSLATE_PLURAL_FS"

if (( $# > 1 )); then
	echo "Usage: $0 [translation.ts]" >&2
	exit 1
fi

if (( $# == 1 )); then
	filename=$1
	if [[ ! -f "$filename" || "$filename" != *.ts ]]; then
		echo "Error: '$filename' is not an existing .ts file." >&2
		exit 1
	fi
	filename=$(realpath "$filename")
else
	echo "To update an existing translation, pass its .ts file to this script."
	read -r -p "Do you want to create a new translation? (y/n)... " ANSWER
	if [[ "$ANSWER" != "y" && "$ANSWER" != "Y" ]]; then
		exit 0
	fi

	echo
	echo "Enter an ISO 639-1 language code, optionally followed by an ISO 3166 country code."
	echo "Examples: en, en-AU"
	read -r -p "Language code... " LANGUAGE_CODE
	if [[ -z "$LANGUAGE_CODE" || "$LANGUAGE_CODE" == */* ]]; then
		echo "Error: invalid language code." >&2
		exit 1
	fi
	filename="$scriptdir/duckstation-qt_${LANGUAGE_CODE}.ts"
fi

echo "Updating $(basename "$filename")..."
cd "$scriptdir"
# context is intentionally unquoted so each lupdate option is passed separately.
"$linguist/lupdate" $context -ts "$filename"

nohup "$linguist/linguist" "$filename" >/dev/null 2>&1 &
