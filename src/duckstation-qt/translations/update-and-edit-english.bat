@echo off

set "linguist=..\..\..\dep\msvc\qt\5.15.0\msvc2017_64\bin"
set context=../ ../../core/ ../../frontend-common/ -tr-function-alias translate+=TranslateString -tr-function-alias translate+=TranslateStdString -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATABLE -pluralonly

"%linguist%\lupdate.exe" %context% -ts duckstation-qt_en.ts
pause

cd "%linguist%"
start /B linguist.exe "%~dp0\duckstation-qt_en.ts"
