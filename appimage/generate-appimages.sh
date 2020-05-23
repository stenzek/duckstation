#!/bin/bash

# NOTE: Keep this script in the same directory as resources for AppImage creation
APPIMAGE_RESOURCES_DIR=$(dirname $(readlink -f $0))
echo "APPIMAGE_RESOURCES_DIR set to ${APPIMAGE_RESOURCES_DIR}"

if [[ "$#" -ne 1 ]]; then
  echo "Wrong number of arguments (\$# = $# args) provided."
  echo "Usage: create-appimage.sh <build_directory_path>"
  exit 1
else
  BUILD_DIR=$(readlink -f $1)
  echo "BUILD_DIR set to ${BUILD_DIR}"
fi

# Acquire linuxdeploy and linuxdeploy-plugin-qt
wget --timestamping --directory-prefix=${BUILD_DIR} \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
chmod a+x ${BUILD_DIR}/linuxdeploy-x86_64.AppImage

wget --timestamping --directory-prefix=${BUILD_DIR} \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod a+x ${BUILD_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage

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

# Add data files into the AppDir
DATA_DIR=$(dirname $(realpath "${BASH_SOURCE[0]}"))/../data
echo "Data directory is: ${DATA_DIR}"
for frontend in ${FRONTENDS[@]}; do
  CURRENT_APPDIR=${BUILD_DIR}/AppDir-duckstation-${frontend}
  mkdir -p ${CURRENT_APPDIR}/usr/bin
  cp -av ${DATA_DIR}/* ${CURRENT_APPDIR}/usr/bin
done

# Pass UPDATE_INFORMATION and OUTPUT variables (used by linuxdeploy-plugin-appimage)
# to the environment of the linuxdeploy commands

UPDATE_INFORMATION="zsync|https://github.com/stenzek/duckstation/releases/download/latest/duckstation-qt-x64.AppImage.zsync" \
OUTPUT="duckstation-qt-x64.AppImage" \
${BUILD_DIR}/linuxdeploy-x86_64.AppImage \
  --appdir=${BUILD_DIR}/AppDir-duckstation-qt \
  --executable=${BUILD_DIR}/bin/duckstation-qt \
  --desktop-file=${APPIMAGE_RESOURCES_DIR}/duckstation-qt.desktop \
  ${ICONS_QT[@]/#/--icon-file=} \
  --plugin=qt \
  --output=appimage

UPDATE_INFORMATION="zsync|https://github.com/stenzek/duckstation/releases/download/latest/duckstation-sdl-x64.AppImage.zsync" \
OUTPUT="duckstation-sdl-x64.AppImage" \
${BUILD_DIR}/linuxdeploy-x86_64.AppImage \
  --appdir=${BUILD_DIR}/AppDir-duckstation-sdl \
  --executable=${BUILD_DIR}/bin/duckstation-sdl \
  --desktop-file=${APPIMAGE_RESOURCES_DIR}/duckstation-sdl.desktop \
  ${ICONS_SDL[@]/#/--icon-file=} \
  --output=appimage

# Resulting AppImages and zsync files will be created in the directory this script is called from;
# move them into the user's specified build directory
