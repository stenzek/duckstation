@echo off

PUSHD "%~dp0"
FOR /F "tokens=1-3 delims=.-" %%a IN ('git describe --dirty') DO (
  FOR %%f IN (duckstation-qt installer uninstaller) DO (
    powershell -NoProfile -Command ^
      "$file = '..\%%f\%%f.rc';" ^
      "(Get-Content $file) -replace '\b\d+,\d+,\d+,\d+\b', '%%a,%%b,%%c,0' -replace '\b\d+\.\d+\.\d+(?:\.\d+)?\b', '%%a.%%b.%%c' | Set-Content -Encoding ASCII $file"
  )
)
POPD
