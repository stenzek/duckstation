@echo off

if not defined lang (echo Please set your language first & pause & exit)

set "linguist=..\..\..\dep\msvc\deps-x64\bin"
SET "context=.././ ../../core/ ../../util/ -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATE,QT_TRANSLATE_NOOP+=TRANSLATE_SV,QT_TRANSLATE_NOOP+=TRANSLATE_STR,QT_TRANSLATE_NOOP+=TRANSLATE_FS,QT_TRANSLATE_N_NOOP3+=TRANSLATE_FMT,QT_TRANSLATE_NOOP+=TRANSLATE_NOOP"

"%linguist%\lupdate.exe" %context% -noobsolete -ts duckstation-qt_%lang%.ts
pause

cd "%linguist%"
start /B linguist.exe "%~dp0\duckstation-qt_%lang%.ts"
