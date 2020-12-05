@echo off
echo Set your language
echo.
echo Examples:
echo   en    ^<-- English
echo   en-au ^<-- Australian English
echo         ^<-- Remove language setting
echo.
echo For the 369-1 2-digit language code
echo https://en.wikipedia.org/wiki/List_of_ISO_639-1_codes
echo.
echo If you require a country code as well (you probably don't)
echo https://en.wikipedia.org/wiki/List_of_ISO_3166_country_codes
echo.
echo.%lang%
set /p newlang="Enter language code: "
if defined newlang ( setx lang %newlang% )
if defined lang if not defined newlang ( 
  echo Removing language setting...
  setx lang "" 1>nul
  reg delete HKCU\Environment /F /V lang 2>nul
  reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /F /V lang 2>nul
)
pause
