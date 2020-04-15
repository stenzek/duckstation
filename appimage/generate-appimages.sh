#!/bin/bash

# NOTE: Keep this script in the same directory as resources for AppImage creation
APPIMAGE_RESOURCES_DIR=$(dirname $(readlink -f $0))
echo "APPIMAGE_RESOURCES_DIR set to ${APPIMAGE_RESOURCES_DIR}"

if [[ "$#" -ne 1 ]]; then
  echo "Wrong number of arguments (\$# = $# args) provided."
  echo "Usage: create-appimage.sh <build_directory_path>"
  exit 1
else
  BUILD_DIR=$1
  echo "BUILD_DIR set to ${BUILD_DIR}"
fi

# Acquire linuxdeploy and linuxdeploy-plugin-qt
wget -N https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
chmod a+x linuxdeploy-x86_64.AppImage
wget -N https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod a+x linuxdeploy-plugin-qt-x86_64.AppImage

# Copy icons into the <resolution>/<app_name>.<ext> directory structure that linuxdeploy nominally expects,
# e.g. 16x16/duckstation-qt.png, 32x32/duckstation-qt.png, etc.
FRONTENDS=("qt" "sdl")
ICONS_QT=()
ICONS_SDL=()

ICON_PNG_RESOLUTIONS=($(seq 16 16 64)) # 16, 32, 48, 64
for res in ${ICON_PNG_RESOLUTIONS[@]}; do
  mkdir -p ${BUILD_DIR}/AppImage-icons/${res}x${res}
  for frontend in ${FRONTENDS[@]}; do
    # Copy icon to proper directory
    cp ${APPIMAGE_RESOURCES_DIR}/icon-${res}px.png ${BUILD_DIR}/AppImage-icons/${res}x${res}/duckstation-${frontend}.png
    # Append icon filepath to array that will later be passed to linuxdeploy
    eval "ICONS_${frontend^^}+=(${BUILD_DIR}/AppImage-icons/${res}x${res}/duckstation-${frontend}.png)"
  done
done

# Outputted file from linuxdeploy is named based on the .desktop file Name key;
# We rename it to something generic that buildbot or CI scripts can modify
# as they wish outside of this script, e.g. to distinguish between Release or
# Debug builds, since we don't have awareness of that inside this script

./linuxdeploy-x86_64.AppImage --appdir=./AppDir-duckstation-qt \
  --executable=${BUILD_DIR}/src/duckstation-qt/duckstation-qt \
  --desktop-file=${APPIMAGE_RESOURCES_DIR}/duckstation-qt.desktop \
  ${ICONS_QT[@]/#/--icon-file=} \
  --plugin=qt \
  --output=appimage \
  && mv DuckStation_Qt*.AppImage duckstation-qt-x64.AppImage

./linuxdeploy-x86_64.AppImage --appdir=./AppDir-duckstation-sdl \
  --executable=${BUILD_DIR}/src/duckstation-sdl/duckstation-sdl \
  --desktop-file=${APPIMAGE_RESOURCES_DIR}/duckstation-sdl.desktop \
  ${ICONS_SDL[@]/#/--icon-file=} \
  --output=appimage \
  && mv DuckStation_SDL*.AppImage duckstation-sdl-x64.AppImage

# Resulting AppImage files will be located in the directory this script is called from
