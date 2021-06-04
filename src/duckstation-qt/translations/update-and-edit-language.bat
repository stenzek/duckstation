@echo off

if not defined lang (echo Please set your language first & pause & exit)

set "linguist=..\..\..\dep\msvc\qt\6.1.0\msvc2019_64\bin"
set context=../ ../../core/ ../../frontend-common/ -tr-function-alias translate+=TranslateString -tr-function-alias translate+=TranslateStdString -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATABLE

"%linguist%\lupdate.exe" %context% -ts duckstation-qt_%lang%.ts
pause

cd "%linguist%"
start /B linguist.exe "%~dp0\duckstation-qt_%lang%.ts"
