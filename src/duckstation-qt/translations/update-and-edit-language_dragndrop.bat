@ECHO OFF
REM Script to provide and easy way to update and edit .ts files for Duckstation.
REM Usage: drag and drop a duckstation .ts file on this batch file
TITLE Duckstation - Update and Edit .ts files (Drag n Drop)

REM Check if an argument is provided
SET arg1="%~1"
IF %arg1%=="" goto noarg

REM get filename.extension and extension separately
FOR %%A IN (%arg1%) DO (
	SET filename=%%~nxA
	SET ext=%%~xA
)

REM Check if the file extension is .ts
IF %ext%==.ts GOTO goodfile

REM The wrong or no file has been passed
:noarg
ECHO Please, drag and drop a .ts file on this batch file to update it and edit it.
ECHO.
PAUSE
EXIT

REM A good .ts file has been passed
:goodfile
ECHO Updating %filename%...
ECHO.
SET "linguist=..\..\..\dep\msvc\qt\5.15.0\msvc2017_64\bin"
SET "context=.././ ../../core/ ../../frontend-common/ -tr-function-alias translate+=TranslateString -tr-function-alias translate+=TranslateStdString -tr-function-alias QT_TRANSLATE_NOOP+=TRANSLATABLE"

"%linguist%\lupdate.exe" %context% -ts %filename%
ECHO.
PAUSE

CD "%linguist%"
START /B linguist.exe "%~dp0\%filename%"

REM References
REM https://stackoverflow.com/questions/9252980/how-to-split-the-filename-from-a-full-path-in-batch#9253018
REM https://stackoverflow.com/questions/26551/how-can-i-pass-arguments-to-a-batch-file
REM https://stackoverflow.com/questions/14786623/batch-file-copy-using-1-for-drag-and-drop
