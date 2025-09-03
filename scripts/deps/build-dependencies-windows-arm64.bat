@echo off
setlocal enabledelayedexpansion

rem SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
rem SPDX-License-Identifier: CC-BY-NC-ND-4.0

echo Setting environment...
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsamd64_arm64.bat" (
  call "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsamd64_arm64.bat"
) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_arm64.bat" (
  call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_arm64.bat"
) else (
  echo Visual Studio 2022 not found.
  goto error
)

set SEVENZIP="C:\Program Files\7-Zip\7z.exe"
set PATCH="C:\Program Files\Git\usr\bin\patch.exe"

if defined DEBUG (
  echo DEBUG=%DEBUG%
) else (
  set DEBUG=1
)

pushd %~dp0
set "SCRIPTDIR=%CD%"
cd ..\..\dep\msvc
mkdir deps-build
cd deps-build || goto error
set "BUILDDIR=%CD%"
cd ..
mkdir deps-arm64
cd deps-arm64 || goto error
set "INSTALLDIR=%CD%"
cd ..
cd deps-x64 || goto error
set "X64INSTALLDIR=%CD%"
cd ..
popd

echo SCRIPTDIR=%SCRIPTDIR%
echo BUILDDIR=%BUILDDIR%
echo INSTALLDIR=%INSTALLDIR%

cd "%BUILDDIR%"

REM Read each line from the file and set as environment variable
for /f "usebackq tokens=1,2 delims==" %%a in ("%SCRIPTDIR%\versions") do (
    if not "%%a"=="" if not "%%b"=="" (
        set "%%a=%%b"
        rem echo Set %%a=%%b
    )
)

call :downloadfile "freetype-%FREETYPE%.tar.gz" "https://download.savannah.gnu.org/releases/freetype/freetype-%FREETYPE%.tar.gz" "%FREETYPE_GZ_HASH%" || goto error
call :downloadfile "harfbuzz-%HARFBUZZ%.tar.gz" "https://github.com/harfbuzz/harfbuzz/archive/refs/tags/%HARFBUZZ%.tar.gz" "%HARFBUZZ_GZ_HASH%" || goto error
call :downloadfile "libpng-%LIBPNG%.tar.gz" "https://download.sourceforge.net/libpng/libpng-%LIBPNG%.tar.gz" "%LIBPNG_GZ_HASH%" || goto error
call :downloadfile "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/%LIBJPEGTURBO%/libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" "%LIBJPEGTURBO_GZ_HASH%" || goto error
call :downloadfile "SDL3-%SDL3%.zip" "https://github.com/libsdl-org/SDL/releases/download/release-%SDL3%/SDL3-%SDL3%.zip" "%SDL3_ZIP_HASH%" || goto error
call :downloadfile "qtbase-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qtbase-everywhere-src-%QT%.zip" "%QTBASE_ZIP_HASH%" || goto error
call :downloadfile "qtimageformats-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qtimageformats-everywhere-src-%QT%.zip" "%QTIMAGEFORMATS_ZIP_HASH%" || goto error
call :downloadfile "qtsvg-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qtsvg-everywhere-src-%QT%.zip" "%QTSVG_ZIP_HASH%" || goto error
call :downloadfile "qttools-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qttools-everywhere-src-%QT%.zip" "%QTTOOLS_ZIP_HASH%" || goto error
call :downloadfile "qttranslations-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qttranslations-everywhere-src-%QT%.zip" "%QTTRANSLATIONS_ZIP_HASH%" || goto error
call :downloadfile "libwebp-%LIBWEBP%.tar.gz" "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-%LIBWEBP%.tar.gz" "%LIBWEBP_GZ_HASH%" || goto error
call :downloadfile "libzip-%LIBZIP%.tar.gz" "https://github.com/nih-at/libzip/releases/download/v%LIBZIP%/libzip-%LIBZIP%.tar.gz" "%LIBZIP_GZ_HASH%" || goto error
call :downloadfile "zlib-ng-%ZLIBNG%.tar.gz" "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/%ZLIBNG%.tar.gz" "%ZLIBNG_GZ_HASH%" || goto error
call :downloadfile "zstd-%ZSTD%.tar.gz" "https://github.com/facebook/zstd/releases/download/v%ZSTD%/zstd-%ZSTD%.tar.gz" "%ZSTD_GZ_HASH%" || goto error

call :downloadfile "cpuinfo-%CPUINFO%.tar.gz" "https://github.com/stenzek/cpuinfo/archive/%CPUINFO%.tar.gz" "%CPUINFO_GZ_HASH%" || goto error
call :downloadfile "discord-rpc-%DISCORD_RPC%.tar.gz" "https://github.com/stenzek/discord-rpc/archive/%DISCORD_RPC%.tar.gz" "%DISCORD_RPC_GZ_HASH%" || goto error
call :downloadfile "plutosvg-%PLUTOSVG%.tar.gz" "https://github.com/stenzek/plutosvg/archive/%PLUTOSVG%.tar.gz" "%PLUTOSVG_GZ_HASH%" || goto error
call :downloadfile "shaderc-%SHADERC%.tar.gz" "https://github.com/stenzek/shaderc/archive/%SHADERC%.tar.gz" "%SHADERC_GZ_HASH%" || goto error
call :downloadfile "soundtouch-%SOUNDTOUCH%.tar.gz" "https://github.com/stenzek/soundtouch/archive/%SOUNDTOUCH%.tar.gz" "%SOUNDTOUCH_GZ_HASH%" || goto error
call :downloadfile "dxcompiler-%DXCOMPILER%.zip" "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DXC/%DXCOMPILER%" "%DXCOMPILER_ZIP_HASH%" || goto error

if not exist SPIRV-Cross\ (
  git clone https://github.com/KhronosGroup/SPIRV-Cross/ -b %SPIRV_CROSS% --depth 1 || goto error
  pushd SPIRV-Cross
  git reset --hard %SPIRV_CROSS_SHA% || goto error
  popd
)

if %DEBUG%==1 (
  echo Building debug and release libraries...
) else (
  echo Building release libraries...
)

set FORCEPDB=-DCMAKE_SHARED_LINKER_FLAGS_RELEASE="/DEBUG"
set ARM64TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE="%SCRIPTDIR%\cmake-toolchain-windows-arm64.cmake"

echo Building zlib-ng...
rmdir /S /Q "zlib-ng-%ZLIBNG%"
tar -xf "zlib-ng-%ZLIBNG%.tar.gz" || goto error
cd "zlib-ng-%ZLIBNG%" || goto error
rem BUILD_SHARED_LIBS deliberately ommitted so that both shared and static libraries are built, we need static for the updater.
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DZLIB_COMPAT=ON -DZLIBNG_ENABLE_TESTS=OFF -DZLIB_ENABLE_TESTS=OFF -DWITH_GTEST=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libpng...
rmdir /S /Q "libpng-%LIBPNG%"
tar -xf "libpng-%LIBPNG%.tar.gz" || goto error
cd "libpng-%LIBPNG%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libjpeg...
rmdir /S /Q "libjpeg-turbo-%LIBJPEGTURBO%"
tar -xf "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" || goto error
cd "libjpeg-turbo-%LIBJPEGTURBO%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DENABLE_STATIC=OFF -DENABLE_SHARED=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building Zstandard...
rmdir /S /Q "zstd-%ZSTD%"
tar -xf "zstd-%ZSTD%.tar.gz" --exclude "zstd-%ZSTD%/tests/cli-tests/*" || goto error
cd "zstd-%ZSTD%"
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building WebP...
rmdir /S /Q "libwebp-%LIBWEBP%"
tar -xf "libwebp-%LIBWEBP%.tar.gz" || goto error
cd "libwebp-%LIBWEBP%" || goto error
cmake -B build %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libzip...
rmdir /S /Q "libzip-%LIBZIP%"
tar -xf "libzip-%LIBZIP%.tar.gz" || goto error
cd "libzip-%LIBZIP%" || goto error
cmake -B build %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=ON -DBUILD_SHARED_LIBS=ON -DLIBZIP_DO_INSTALL=ON -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building FreeType without HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building HarfBuzz...
rmdir /S /Q "harfbuzz-%HARFBUZZ%"
tar -xf "harfbuzz-%HARFBUZZ%.tar.gz" || goto error
cd "harfbuzz-%HARFBUZZ%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DHB_BUILD_UTILS=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building FreeType with HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_REQUIRE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building SDL...
rmdir /S /Q "SDL3-%SDL3%"
%SEVENZIP% x "SDL3-%SDL3%.zip" || goto error
cd "SDL3-%SDL3%" || goto error
cmake -B build %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release %FORCEPDB% -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
copy build\SDL3.pdb "%INSTALLDIR%\bin" || goto error
cd .. || goto error

if %DEBUG%==1 (
  set QTBUILDSPEC=-DCMAKE_CONFIGURATION_TYPES="Release;Debug" -G "Ninja Multi-Config"
) else (
  set QTBUILDSPEC=-DCMAKE_BUILD_TYPE=Release -G Ninja
)

echo Building Qt base...
rmdir /S /Q "qtbase-everywhere-src-%QT%"
%SEVENZIP% x "qtbase-everywhere-src-%QT%.zip" || goto error
cd "qtbase-everywhere-src-%QT%" || goto error

rem Stop checkboxes in Fusion theme having such bright outlines.
%PATCH% -p1 < "%SCRIPTDIR%\qtbase-fusion-style.patch" || goto error

cmake -B build %ARM64TOOLCHAIN% -DFEATURE_sql=OFF -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DQT_HOST_PATH="%X64INSTALLDIR%" %FORCEPDB% -DQT_GENERATE_SBOM=OFF -DINPUT_ssl=yes -DINPUT_openssl=no -DFEATURE_system_png=ON -DFEATURE_system_jpeg=ON -DFEATURE_system_zlib=ON -DFEATURE_system_freetype=ON -DFEATURE_system_harfbuzz=ON %QTBUILDSPEC% || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building Qt SVG...
rmdir /S /Q "qtsvg-everywhere-src-%QT%"
%SEVENZIP% x "qtsvg-everywhere-src-%QT%.zip" || goto error
cd "qtsvg-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DQT_GENERATE_SBOM=OFF || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Image Formats...
rmdir /S /Q "qtimageformats-everywhere-src-%QT%"
%SEVENZIP% x "qtimageformats-everywhere-src-%QT%.zip" || goto error
cd "qtimageformats-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DQT_GENERATE_SBOM=OFF -DFEATURE_system_webp=ON || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Tools...
rmdir /S /Q "qtimageformats-everywhere-src-%QT%"
%SEVENZIP% x "qttools-everywhere-src-%QT%.zip" || goto error
cd "qttools-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DQT_GENERATE_SBOM=OFF -DFEATURE_assistant=OFF -DFEATURE_clang=OFF -DFEATURE_designer=OFF -DFEATURE_kmap2qmap=OFF -DFEATURE_pixeltool=OFF -DFEATURE_pkg_config=OFF -DFEATURE_qev=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Translations...
rmdir /S /Q "qttranslations-everywhere-src-%QT%"
%SEVENZIP% x "qttranslations-everywhere-src-%QT%.zip" || goto error
cd "qttranslations-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DQT_GENERATE_SBOM=OFF || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building shaderc...
rmdir /S /Q "shaderc-%SHADERC%"
tar -xf "shaderc-%SHADERC%.tar.gz" || goto error
cd "shaderc-%SHADERC%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_SHARED_CRT=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building SPIRV-Cross...
cd SPIRV-Cross || goto error
rmdir /S /Q "build"
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=ON -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=ON -DSPIRV_CROSS_ENABLE_UTIL=ON -B build -G Ninja
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building cpuinfo...
rmdir /S /Q "cpuinfo-%CPUINFO%"
tar -xf "cpuinfo-%CPUINFO%.tar.gz" || goto error
cd "cpuinfo-%CPUINFO%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DCPUINFO_LIBRARY_TYPE=shared -DCPUINFO_RUNTIME_TYPE=shared -DCPUINFO_LOG_LEVEL=error -DCPUINFO_LOG_TO_STDIO=ON -DCPUINFO_BUILD_TOOLS=OFF -DCPUINFO_BUILD_UNIT_TESTS=OFF -DCPUINFO_BUILD_MOCK_TESTS=OFF -DCPUINFO_BUILD_BENCHMARKS=OFF -DUSE_SYSTEM_LIBS=ON -B build -G Ninja
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building discord-rpc...
rmdir /S /Q "discord-rpc-%DISCORD_RPC%"
tar -xf "discord-rpc-%DISCORD_RPC%.tar.gz" || goto error
cd "discord-rpc-%DISCORD_RPC%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -B build -G Ninja
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building plutosvg...
rmdir /S /Q "plutosvg-%PLUTOSVG%"
tar -xf "plutosvg-%PLUTOSVG%.tar.gz" || goto error
cd "plutosvg-%PLUTOSVG%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build -G Ninja
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

rem This currently isn't using clang-cl. It probably should, might be losing a little speed.
echo Building soundtouch...
rmdir /S /Q "soundtouch-%SOUNDTOUCH%"
tar -xf "soundtouch-%SOUNDTOUCH%.tar.gz" || goto error
cd "soundtouch-%SOUNDTOUCH%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

rem These should already exist, but just in case.
mkdir "%INSTALLDIR%\bin"
mkdir "%INSTALLDIR%\include"
mkdir "%INSTALLDIR%\lib"

echo Extracting DXCompiler...
rmdir /S /Q "dxcompiler-%DXCOMPILER%"
mkdir "dxcompiler-%DXCOMPILER%"
cd "dxcompiler-%DXCOMPILER%" || goto error
%SEVENZIP% x "..\dxcompiler-%DXCOMPILER%.zip" || goto error
copy build\native\include\* "%INSTALLDIR%\include" || goto error
copy build\native\bin\arm64\*.dll "%INSTALLDIR%\bin" || goto error
copy build\native\lib\arm64\*.lib "%INSTALLDIR%\lib" || goto error
cd .. || goto error

echo Cleaning up...
cd ..
rd /S /Q deps-build

echo Exiting with success.
exit 0

:error
echo Failed with error #%errorlevel%.
pause
exit %errorlevel%

:downloadfile
if not exist "%~1" (
  echo Downloading %~1 from %~2...
  curl -L -o "%~1" "%~2" || goto error
)

rem based on https://gist.github.com/gsscoder/e22daefaff9b5d8ac16afb070f1a7971
set idx=0
for /f %%F in ('certutil -hashfile "%~1" SHA256') do (
    set "out!idx!=%%F"
    set /a idx += 1
)
set filechecksum=%out1%

if /i %~3==%filechecksum% (
    echo Validated %~1.
    exit /B 0
) else (
    echo Expected %~3 got %filechecksum%.
    exit /B 1
)
