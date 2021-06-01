@echo off
echo Updating SCM version...
pushd %~dp0
cd ..\src\scmversion
start /w gen_scmversion.bat
popd

echo Setting MSVC environment...
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

echo Creating build directory...
pushd %~dp0
cd ..
if not exist build-libretro mkdir build-libretro
cd build-libretro
del /q duckstation_libretro_windows_x64.zip
rmdir /Q /S windows_x64
mkdir windows_x64
cd windows_x64

echo Running CMake...
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRETRO_CORE=ON -DCMAKE_C_COMPILER:FILEPATH="%VCToolsInstallDir%\bin\HostX64\x64\cl.exe" -DCMAKE_CXX_COMPILER:FILEPATH="%VCToolsInstallDir%\bin\HostX64\x64\cl.exe" ..\..

echo Building...
ninja
if %errorlevel% neq 0 exit /b %errorlevel%

echo Zipping...
"C:\Program Files\7-Zip\7z.exe" a ../duckstation_libretro_windows_x64.zip ./duckstation_libretro.dll

echo All done.
