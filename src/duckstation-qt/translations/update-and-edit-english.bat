@echo off

set "linguist=..\..\..\dep\msvc\deps-x64\bin"
set context=../ ../../core/ ../../util/ -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATE,QT_TRANSLATE_NOOP+=TRANSLATE_SV,QT_TRANSLATE_NOOP+=TRANSLATE_STR,QT_TRANSLATE_NOOP+=TRANSLATE_FS,QT_TRANSLATE_N_NOOP3+=TRANSLATE_FMT,QT_TRANSLATE_NOOP+=TRANSLATE_NOOP -pluralonly

"%linguist%\lupdate.exe" %context% -ts duckstation-qt_en.ts
pause

cd "%linguist%"
start /B linguist.exe "%~dp0\duckstation-qt_en.ts"
