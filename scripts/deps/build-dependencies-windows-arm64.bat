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

set FREETYPE=2.13.3
set HARFBUZZ=10.4.0
set LIBJPEGTURBO=3.1.0
set LIBPNG=1647
set QT=6.9.0
set QTMINOR=6.9
set SDL3=3.2.10
set WEBP=1.5.0
set LIBZIP=1.11.3
set ZLIBNG=2.2.4
set ZSTD=1.5.7

set CPUINFO=3ebbfd45645650c4940bf0f3b4d25ab913466bb0
set DISCORD_RPC=cc59d26d1d628fbd6527aac0ac1d6301f4978b92
set PLUTOSVG=bc845bb6b6511e392f9e1097b26f70cf0b3c33be
set SHADERC=4daf9d466ad00897f755163dd26f528d14e1db44
set SOUNDTOUCH=463ade388f3a51da078dc9ed062bf28e4ba29da7
set SPIRV_CROSS=vulkan-sdk-1.4.309.0
set DXCOMPILER=1.8.2407.12
set DXAGILITY=1.614.1

call :downloadfile "freetype-%FREETYPE%.tar.gz" "https://download.savannah.gnu.org/releases/freetype/freetype-%FREETYPE%.tar.gz" 5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747 || goto error
call :downloadfile "harfbuzz-%HARFBUZZ%.zip" "https://github.com/harfbuzz/harfbuzz/archive/refs/tags/%HARFBUZZ%.zip" 1af1d03abfef15e556ba7bee2ef3b62643387836affffb66f458ce74dcbeba04 || goto error
call :downloadfile "lpng%LIBPNG%.zip" "https://download.sourceforge.net/libpng/lpng%LIBPNG%.zip" 883340209fccd5353e406a79e22a2d226fc61666b83e700ab1e367bc1a679101 || goto error
call :downloadfile "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/%LIBJPEGTURBO%/libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" 9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93 || goto error
call :downloadfile "SDL3-%SDL3%.zip" "https://github.com/libsdl-org/SDL/releases/download/release-%SDL3%/SDL3-%SDL3%.zip" 01d9ab20fc071b076be91df5396b464b4ef159e93b2b2addda1cc36750fc1f29 || goto error
call :downloadfile "qtbase-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qtbase-everywhere-src-%QT%.zip" 513df15a6365a40f6230ec9463ad8c71b824e181d4b661dac9707e103b24ae0c || goto error
call :downloadfile "qtimageformats-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qtimageformats-everywhere-src-%QT%.zip" d428fd17a0d3f92c48a30f1d23806bf20352fbce2e80e5bbee27fa80576480ee || goto error
call :downloadfile "qtsvg-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qtsvg-everywhere-src-%QT%.zip" 54bf06afeb67035f1c6afcd00beec755c0d776626b4cce9ab56992a55215ba69 || goto error
call :downloadfile "qttools-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qttools-everywhere-src-%QT%.zip" 5f8a94a161bd2e71a82f478dc19f4ec77ac95a50709f5a68d5951001ed6bb856 || goto error
call :downloadfile "qttranslations-everywhere-src-%QT%.zip" "https://download.qt.io/official_releases/qt/%QTMINOR%/%QT%/submodules/qttranslations-everywhere-src-%QT%.zip" 5885ce1a114615cc5fa69e459f069d3fe2bcb1320fd9cc162821f3920ef44735 || goto error
call :downloadfile "libwebp-%WEBP%.tar.gz" "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-%WEBP%.tar.gz" 7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c || goto error
call :downloadfile "libzip-%LIBZIP%.tar.gz" "https://github.com/nih-at/libzip/releases/download/v%LIBZIP%/libzip-%LIBZIP%.tar.gz" 76653f135dde3036036c500e11861648ffbf9e1fc5b233ff473c60897d9db0ea || goto error
call :downloadfile "zlib-ng-%ZLIBNG%.zip" "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/%ZLIBNG%.zip" 5e78f0ebbe507fe294bf756c741a8af4766d3838c54460a087e906b3f20346e4 || goto error
call :downloadfile "zstd-%ZSTD%.zip" "https://github.com/facebook/zstd/archive/refs/tags/v%ZSTD%.zip" 7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4 || goto error

call :downloadfile "cpuinfo-%CPUINFO%.zip" "https://github.com/stenzek/cpuinfo/archive/%CPUINFO%.zip" 3430f8bae57623347b2b30a8ff041b0288f90ad99b4c2487c3d520863ce4a4e3 || goto error
call :downloadfile "discord-rpc-%DISCORD_RPC%.zip" "https://github.com/stenzek/discord-rpc/archive/%DISCORD_RPC%.zip" 4492cbe690a16546da9a9d89f14340352cad3b0ac5484b969749d6ab6f1ee836 || goto error
call :downloadfile "plutosvg-%PLUTOSVG%.zip" "https://github.com/stenzek/plutosvg/archive/%PLUTOSVG%.zip" ae6c6bd93a9ea0451b853545595b2c6c99104b22c193afd8e00cfbc0f3e27298 || goto error
call :downloadfile "shaderc-%SHADERC%.zip" "https://github.com/stenzek/shaderc/archive/%SHADERC%.zip" cafd87502d799060d2b6bbf9c885226f75f6e0b21e8cd87a43806da019412811 || goto error
call :downloadfile "soundtouch-%SOUNDTOUCH%.zip" "https://github.com/stenzek/soundtouch/archive/%SOUNDTOUCH%.zip" 107a1941181a69abe28018b9ad26fc0218625758ac193bc979abc9e26b7c0c3a || goto error
call :downloadfile "dxcompiler-%DXCOMPILER%.zip" "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DXC/%DXCOMPILER%" eb4f6a3bb6b08aaa62f435b3dbf26b180702ca52398d3650d0dd538f56742cdc || goto error
call :downloadfile "dxagility-%DXAGILITY%.zip" "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/%DXAGILITY%" 9880aa91602dd51dd6cf7911a2bca7a2323513b15338573cde014b3356eeaff2 || goto error

if not exist SPIRV-Cross\ (
  git clone https://github.com/KhronosGroup/SPIRV-Cross/ -b %SPIRV_CROSS% --depth 1 || goto error
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
%SEVENZIP% x "zlib-ng-%ZLIBNG%.zip" || goto error
cd "zlib-ng-%ZLIBNG%" || goto error
rem BUILD_SHARED_LIBS deliberately ommitted so that both shared and static libraries are built, we need static for the updater.
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DZLIB_COMPAT=ON -DZLIBNG_ENABLE_TESTS=OFF -DZLIB_ENABLE_TESTS=OFF -DWITH_GTEST=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libpng...
rmdir /S /Q "lpng%LIBPNG%"
%SEVENZIP% x "lpng%LIBPNG%.zip" || goto error
cd "lpng%LIBPNG%" || goto error
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
%SEVENZIP% x "-x^!zstd-1.5.6\tests\cli-tests\bin" "zstd-%ZSTD%.zip" || goto error
cd "zstd-%ZSTD%"
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building WebP...
rmdir /S /Q "libwebp-%WEBP%"
tar -xf "libwebp-%WEBP%.tar.gz" || goto error
cd "libwebp-%WEBP%" || goto error
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
%SEVENZIP% x "-x^!harfbuzz-%HARFBUZZ%\README" "harfbuzz-%HARFBUZZ%.zip" || goto error
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

rem Disable the PCRE2 JIT, it doesn't properly verify AVX2 support.
%PATCH% -p1 < "%SCRIPTDIR%\qtbase-disable-pcre2-jit.patch" || goto error

cmake -B build %ARM64TOOLCHAIN% -DFEATURE_sql=OFF -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DQT_HOST_PATH="%X64INSTALLDIR%" %FORCEPDB% -DINPUT_gui=yes -DINPUT_widgets=yes -DINPUT_ssl=yes -DINPUT_openssl=no -DINPUT_schannel=yes -DFEATURE_system_png=ON -DFEATURE_system_jpeg=ON -DFEATURE_system_zlib=ON -DFEATURE_system_freetype=ON -DFEATURE_system_harfbuzz=ON %QTBUILDSPEC% || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building Qt SVG...
rmdir /S /Q "qtsvg-everywhere-src-%QT%"
%SEVENZIP% x "qtsvg-everywhere-src-%QT%.zip" || goto error
cd "qtsvg-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Image Formats...
rmdir /S /Q "qtimageformats-everywhere-src-%QT%"
%SEVENZIP% x "qtimageformats-everywhere-src-%QT%.zip" || goto error
cd "qtimageformats-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DFEATURE_system_webp=ON || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Tools...
rmdir /S /Q "qtimageformats-everywhere-src-%QT%"
%SEVENZIP% x "qttools-everywhere-src-%QT%.zip" || goto error
cd "qttools-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DFEATURE_assistant=OFF -DFEATURE_clang=OFF -DFEATURE_designer=OFF -DFEATURE_kmap2qmap=OFF -DFEATURE_pixeltool=OFF -DFEATURE_pkg_config=OFF -DFEATURE_qev=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Translations...
rmdir /S /Q "qttranslations-everywhere-src-%QT%"
%SEVENZIP% x "qttranslations-everywhere-src-%QT%.zip" || goto error
cd "qttranslations-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building shaderc...
rmdir /S /Q "shaderc-%SHADERC%"
%SEVENZIP% x "shaderc-%SHADERC%.zip" || goto error
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
%SEVENZIP% x "cpuinfo-%CPUINFO%.zip" || goto error
cd "cpuinfo-%CPUINFO%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DCPUINFO_LIBRARY_TYPE=shared -DCPUINFO_RUNTIME_TYPE=shared -DCPUINFO_LOG_LEVEL=error -DCPUINFO_LOG_TO_STDIO=ON -DCPUINFO_BUILD_TOOLS=OFF -DCPUINFO_BUILD_UNIT_TESTS=OFF -DCPUINFO_BUILD_MOCK_TESTS=OFF -DCPUINFO_BUILD_BENCHMARKS=OFF -DUSE_SYSTEM_LIBS=ON -B build -G Ninja
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building discord-rpc...
rmdir /S /Q "discord-rpc-%DISCORD_RPC%"
%SEVENZIP% x "discord-rpc-%DISCORD_RPC%.zip" || goto error
cd "discord-rpc-%DISCORD_RPC%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -B build -G Ninja
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building plutosvg...
rmdir /S /Q "plutosvg-%PLUTOSVG%"
%SEVENZIP% x "plutosvg-%PLUTOSVG%.zip" || goto error
cd "plutosvg-%PLUTOSVG%" || goto error
cmake %ARM64TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build -G Ninja
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

rem This currently isn't using clang-cl. It probably should, might be losing a little speed.
echo Building soundtouch...
rmdir /S /Q "soundtouch-%SOUNDTOUCH%"
%SEVENZIP% x "soundtouch-%SOUNDTOUCH%.zip" || goto error
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

echo Extracting DXAgility...
rmdir /S /Q "dxagility-%DXAGILITY%"
mkdir "dxagility-%DXAGILITY%"
cd "dxagility-%DXAGILITY%" || goto error
%SEVENZIP% x "..\dxagility-%DXAGILITY%.zip" || goto error
xcopy /S /Y build\native\include\* "%INSTALLDIR%\include" || goto error
copy build\native\bin\arm64\*.dll "%INSTALLDIR%\bin" || goto error
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
