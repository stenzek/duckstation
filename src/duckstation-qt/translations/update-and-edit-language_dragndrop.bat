@echo off

REM References
REM https://stackoverflow.com/questions/9252980/how-to-split-the-filename-from-a-full-path-in-batch#9253018
REM https://stackoverflow.com/questions/26551/how-can-i-pass-arguments-to-a-batch-file
REM https://stackoverflow.com/questions/14786623/batch-file-copy-using-1-for-drag-and-drop

SET arg1="%~1"
IF %arg1%=="" EXIT
FOR %%A IN (%arg1%) DO (set filename=%%~nxA)

set "linguist=..\..\..\dep\msvc\qt\5.15.0\msvc2017_64\bin"
set "context=.././ ../../core/ ../../frontend-common/ -tr-function-alias translate+=TranslateString -tr-function-alias translate+=TranslateStdString -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATABLE"

"%linguist%\lupdate.exe" %context% -ts %filename%
pause

cd "%linguist%"
start /B linguist.exe "%~dp0\%filename%"
