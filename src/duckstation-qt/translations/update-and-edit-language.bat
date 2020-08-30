@echo off

if not defined lang (echo Please set your language first & pause & exit)

set "linguist=..\..\..\dep\msvc\qt\5.15.0\msvc2017_64\bin"
set context=..\

"%linguist%\lupdate.exe" %context% -ts duckstation-qt_%lang%.ts
pause

cd "%linguist%"
start /B linguist.exe "%~dp0\duckstation-qt_%lang%.ts"
