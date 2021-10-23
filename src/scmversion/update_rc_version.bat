@echo off

FOR /F "tokens=* USEBACKQ" %%g IN (`git describe --tags --dirty --exclude latest --exclude preview --exclude play-store-release`) do (SET "TAG=%%g")

powershell -Command "(gc ..\duckstation-qt\duckstation-qt.rc) -replace '1,0,0,1', '"%TAG:~0,1%","%TAG:~2,1%","%TAG:~4,4%",0' | Out-File -encoding ASCII ..\duckstation-qt\duckstation-qt.rc"
powershell -Command "(gc ..\duckstation-qt\duckstation-qt.rc) -replace '1.0.0.1', '"%TAG:~0,1%"."%TAG:~2,1%"."%TAG:~4,4%"' | Out-File -encoding ASCII ..\duckstation-qt\duckstation-qt.rc"

powershell -Command "(gc ..\duckstation-nogui\duckstation-nogui.rc) -replace '1,0,0,1', '"%TAG:~0,1%","%TAG:~2,1%","%TAG:~4,4%",0' | Out-File -encoding ASCII ..\duckstation-nogui\duckstation-nogui.rc
powershell -Command "(gc ..\duckstation-nogui\duckstation-nogui.rc) -replace '1.0.0.1', '"%TAG:~0,1%"."%TAG:~2,1%"."%TAG:~4,4%"' | Out-File -encoding ASCII ..\duckstation-nogui\duckstation-nogui.rc"