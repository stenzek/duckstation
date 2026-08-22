#!/usr/bin/env bash

set -e

scriptdir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
linguist="$scriptdir/../../../dep/prebuilt/linux-x64/bin"
context=".././ ../../core/ ../../util/ -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATE,QT_TRANSLATE_NOOP+=TRANSLATE_SV,QT_TRANSLATE_NOOP+=TRANSLATE_STR,QT_TRANSLATE_NOOP+=TRANSLATE_FS,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_SV,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_STR,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_FS,QT_TRANSLATE_N_NOOP3+=TRANSLATE_PLURAL_NOOP,QT_TRANSLATE_NOOP+=TRANSLATE_NOOP,QT_TRANSLATE_NOOP3+=TRANSLATE_DISAMBIG_NOOP,translate+=TRANSLATE_PLURAL_STR,translate+=TRANSLATE_PLURAL_SSTR,translate+=TRANSLATE_PLURAL_FS -pluralonly -no-obsolete -locations none"

cd "$scriptdir"
# context is intentionally unquoted so each lupdate option is passed separately.
"$linguist/lupdate" $context -ts duckstation-qt_en.ts

nohup "$linguist/linguist" "$scriptdir/duckstation-qt_en.ts" >/dev/null 2>&1 &
