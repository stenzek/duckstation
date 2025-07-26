# SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0

# NOTE: These package files are intended for your own personal/private use.
# You do not have permission to upload them independently to other package repositories.
#
# To build:
#  git clone https://github.com/stenzek/duckstation.git
#  rpmbuild -bb scripts/packaging/fedora/duckstation.spec
#  sudo rpm -ivh ~/rpmbuild/RPMS/duckstation-*.rpm

Name:    duckstation
Version: 0.0.0
Release: 0%{?dist}
Summary: Fast PlayStation 1 Emulator
License: CC-BY-NC-ND-4.0

BuildRequires: alsa-lib-devel, clang, cmake, dbus-devel, egl-wayland-devel, extra-cmake-modules, gcc-c++
BuildRequires: extra-cmake-modules, freetype-devel, libavcodec-free-devel, libavformat-free-devel
BuildRequires: libavutil-free-devel, libcurl-devel, libevdev-devel, libswresample-free-devel
BuildRequires: libswscale-free-devel, libpng-devel, libwebp-devel, libX11-devel, libXrandr-devel
BuildRequires: libzip-devel, libzip-tools, libzstd-devel, lld, llvm, make, mesa-libEGL-devel, mesa-libGL-devel
BuildRequires: ninja-build, patch, pipewire-devel pulseaudio-libs-devel, wayland-devel, zlib-devel
BuildRequires: qt6-qtbase-devel, qt6-qtbase-private-devel, qt6-qttools, qt6-qttools-devel

Requires: bash curl dbus freetype libpng libwebp libzip libzstd
Requires: qt6-qtbase qt6-qtbase-gui qt6-qtimageformats qt6-qtsvg

# Don't want extra flags producing a slower build than our other formats.
%undefine _hardened_build
%undefine _annotated_build
%undefine _fortify_level
%undefine _include_frame_pointers

# Defines -O2, -flto, and others. We manage LTO ourselves.
%global _general_options "-O3" "-pipe"
%global _preprocessor_defines ""

# We include debug information in the main package for user backtrace reporting.
%global debug_package %{nil}

Source0: %{expand:%%(pwd)}

%description
DuckStation is an simulator/emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability. The goal is to be as accurate as possible while maintaining performance suitable for low-end devices.
"Hack" options are discouraged, the default configuration should support all playable games with only some of the enhancements having compatibility issues.
"PlayStation" and "PSX" are registered trademarks of Sony Interactive Entertainment Europe Limited. This project is not affiliated in any way with Sony Interactive Entertainment.

%prep
%setup -n duckstation -c -T
git clone file://%{SOURCEURL0} .
curl -L -o "data/resources/cheats.zip" "https://github.com/duckstation/chtdb/releases/download/latest/cheats.zip"
curl -L -o "data/resources/patches.zip" "https://github.com/duckstation/chtdb/releases/download/latest/patches.zip"
if [ -f "%{SOURCEURL0}/src/scmversion/tag.h" ]; then
   echo "Copying SCM release tag..."
   cp "%{SOURCEURL0}/src/scmversion/tag.h" src/scmversion
fi

%build

if [ ! -d "${PWD}/deps" ]; then
  scripts/deps/build-dependencies-linux.sh -system-freetype -system-harfbuzz -system-libjpeg -system-libpng -system-libwebp -system-libzip -system-zlib -system-zstd -system-qt "${PWD}/deps"
fi

rm -fr build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=${PWD}/deps \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld" \
    -DCMAKE_MODULE_LINKER_FLAGS_INIT="-fuse-ld=lld" \
    -DCMAKE_SHARED_LINKER_FLAGS_INIT="-fuse-ld=lld" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DCMAKE_INSTALL_PREFIX=%{buildroot}/opt/%{name} \
    -DALLOW_INSTALL=ON
ninja -C build %{?_smp_mflags}

%install
rm -fr %{buildroot}
ninja -C build install
mkdir -p %{buildroot}/usr/bin
ln -s /opt/duckstation/duckstation-qt %{buildroot}/usr/bin/duckstation-qt
install -Dm644 scripts/packaging/org.duckstation.DuckStation.png %{buildroot}/usr/share/icons/hicolor/512x512/apps/org.duckstation.DuckStation.png
install -Dm644 scripts/packaging/org.duckstation.DuckStation.desktop %{buildroot}/usr/share/applications/org.duckstation.DuckStation.desktop

%files
%license LICENSE
/opt/duckstation
/usr/bin/duckstation-qt
/usr/share/icons/hicolor/512x512/apps/org.duckstation.DuckStation.png
/usr/share/applications/org.duckstation.DuckStation.desktop

%changelog

