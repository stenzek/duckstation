@ECHO OFF
REM Script to provide and easy way to update and edit .ts files for Duckstation.
REM Usage: drag and drop a duckstation .ts file on this batch file
REM Author: RaydenX93
REM Credits to Stenzek, Sam Pearman
TITLE Duckstation - Create, and Edit .ts files (Drag n Drop)

REM Check if an argument is provided
SET arg1="%~1"
IF %arg1%=="" GOTO noarg

REM get filename.extension and extension separately
FOR %%A IN (%arg1%) DO (
	SET filename=%%~nxA
	SET ext=%%~xA
)

REM Check if the file extension is .ts
IF %ext%==.ts GOTO goodfile

REM The wrong or no file has been passed
:noarg
ECHO ===================================================================
ECHO Duckstation - Create, Update and Edit .ts files (Drag n Drop)
ECHO ===================================================================
ECHO If you want to update and edit an EXISTING translation, drag and drop a .ts file on this batch file.
ECHO.
ECHO If you want to create a NEW translation, input (y) to start the process.
ECHO. 
SET /P answ=Do you want to create a new translation? (y/n)... 

IF %answ%==y (GOTO newlang) ELSE EXIT 

:newlang
CLS

ECHO Please, insert your language code.
ECHO.
ECHO For the 369-1 2-digit language code:
ECHO https://en.wikipedia.org/wiki/List_of_ISO_639-1_codes
ECHO.
ECHO If you require a country code as well (you probably don't):
ECHO https://en.wikipedia.org/wiki/List_of_ISO_3166_country_codes
ECHO.
ECHO You can select with your mouse and then CTRL^+C to copy the links above.
ECHO.
ECHO Examples:
ECHO   en    ^<-- English
ECHO   en-au ^<-- Australian English
ECHO.

SET /P langcode=Insert your language code... 

CLS
IF NOT DEFINED langcode (
	ECHO Input is invalid. Try again.
	ECHO.
	PAUSE
	GOTO newlang
) ELSE (
	SET filename=duckstation-qt_%langcode%.ts
)

REM A good .ts file has been passed
:goodfile
ECHO Updating %filename%...
ECHO.
SET "linguist=..\..\..\dep\msvc\qt\6.1.0\msvc2019_64\bin"
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
