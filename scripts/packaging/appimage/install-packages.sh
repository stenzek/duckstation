#!/usr/bin/env bash

function retry_command {
  # Package servers tend to be unreliable at times..
  # Retry a bunch of times.
  local RETRIES=10

  for i in $(seq 1 "$RETRIES"); do
    "$@" && break
    if [ "$i" == "$RETRIES" ]; then
      echo "Command \"$@\" failed after ${RETRIES} retries."
      exit 1
    fi
  done
}

# Workaround for https://github.com/actions/runner-images/issues/675
retry_command wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
retry_command sudo apt-add-repository -n 'deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-19 main'

retry_command sudo apt-get update
retry_command sudo apt-get -y install \
  build-essential clang-19 cmake curl extra-cmake-modules git libasound2-dev libcurl4-openssl-dev libdbus-1-dev libdecor-0-dev libegl-dev libevdev-dev \
  libfontconfig-dev libfreetype-dev libfuse2 libgtk-3-dev libgudev-1.0-dev libharfbuzz-dev libinput-dev libopengl-dev libpipewire-0.3-dev libpulse-dev \
  libssl-dev libudev-dev libva-dev libwayland-dev libx11-dev libx11-xcb-dev libxcb1-dev libxcb-composite0-dev libxcb-cursor-dev libxcb-damage0-dev \
  libxcb-glx0-dev libxcb-icccm4-dev libxcb-image0-dev libxcb-keysyms1-dev libxcb-present-dev libxcb-randr0-dev libxcb-render0-dev libxcb-render-util0-dev \
  libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev libxcb-util-dev libxcb-xfixes0-dev libxcb-xinput-dev libxcb-xkb-dev libxext-dev libxkbcommon-x11-dev \
  libxrandr-dev libxss-dev lld-19 llvm-19 nasm ninja-build patchelf pkg-config zlib1g-dev
