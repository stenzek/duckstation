# DuckStation - PlayStation 1, aka. PSX Emulator
[Features](#features) | [Downloading and Running](#downloading-and-running) | [Building](#building) | [Disclaimers](#disclaimers)

**Latest Builds for Windows 10/11 (x64/ARM64), Linux (AppImage/Flatpak), and macOS (11.0+ Universal):** https://github.com/stenzek/duckstation/releases/tag/latest

**Game Compatibility List:** https://docs.google.com/spreadsheets/d/e/2PACX-1vRE0jjiK_aldpICoy5kVQlpk2f81Vo6P4p9vfg4d7YoTOoDlH4PQHoXjTD2F7SdN8SSBLoEAItaIqQo/pubhtml

**Discord Server:** https://www.duckstation.org/discord.html

DuckStation is an simulator/emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability. The goal is to be as accurate as possible while maintaining performance suitable for low-end devices. "Hack" options are discouraged, the default configuration should support all playable games with only some of the enhancements having compatibility issues.

A "BIOS" ROM image is required to to start the emulator and to play games. You can use an image from any hardware version or region, although mismatching game regions and BIOS regions may have compatibility issues. A ROM image is not provided with the emulator for legal reasons, you should dump this from your own console using Caetla or other means.

## Features

DuckStation features a fully-featured frontend built using Qt, as well as a fullscreen/TV UI based on Dear ImGui.

<p align="center">
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main-qt.png" alt="Main Window Screenshot" />
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/bigduck.png" alt="Fullscreen UI Screenshot" />
</p>

Other features include:

 - CPU Recompiler/JIT (x86-64, armv7/AArch32, AArch64, RISC-V/RV64).
 - Hardware (D3D11, D3D12, OpenGL, Vulkan, Metal) and software rendering.
 - Upscaling, texture filtering, and true colour (24-bit) in hardware renderers.
 - PGXP for geometry precision, texture correction, and depth buffer emulation.
 - Adaptive downsampling filter.
 - Post processing shader chains (GLSL and experimental Reshade FX).
 - "Fast boot" for skipping BIOS splash/intro.
 - Save state support.
 - Windows, Linux, macOS support.
 - Supports bin/cue images, raw bin/img files, MAME CHD, single-track ECM, MDS/MDF, and unencrypted PBP formats.
 - Direct booting of homebrew executables.
 - Direct loading of Portable Sound Format (psf) files.
 - Digital and analog controllers for input (rumble is forwarded to host).
 - Namco GunCon lightgun support (simulated with mouse).
 - NeGcon support.
 - Qt and "Big Picture" UI.
 - Automatic updates with preview and latest channels.
 - Automatic content scanning - game titles/hashes are provided by redump.org.
 - Optional automatic switching of memory cards for each game.
 - Supports loading cheats from existing lists.
 - Memory card editor and save importer.
 - Emulated CPU overclocking.
 - Integrated and remote debugging.
 - Multitap controllers (up to 8 devices).
 - RetroAchievements.
 - Automatic loading/applying of PPF patches.

## System Requirements
 - A CPU faster than a potato. But it needs to be x86_64, AArch32/armv7, AArch64/ARMv8, or RISC-V/RV64.
 - For the hardware renderers, a GPU capable of OpenGL 3.1/OpenGL ES 3.1/Direct3D 11 Feature Level 10.0 (or Vulkan 1.0) and above. So, basically anything made in the last 10 years or so.
 - SDL, XInput or DInput compatible game controller (e.g. XB360/XBOne/XBSeries). DualShock 3 users on Windows will need to install the official DualShock 3 drivers included as part of PlayStation Now.

## Downloading and running
Binaries of DuckStation for Windows x64/ARM64, Linux x86_64 (in AppImage/Flatpak formats), and macOS Universal Binaries are available via GitHub Releases and are automatically built with every commit/push. Binaries or packages distributed through other sources may be out of date and are not supported by the developer, please speak to them for support, not us.

### Windows

DuckStation **requires** Windows 10/11, specifically version 1809 or newer. If you are still using Windows 7/8/8.1, DuckStation **will not run** on your operating system. Running these operating systems in 2023 should be considered a security risk, and I would recommend updating to something which receives vendor support.
If you must use an older operating system, [v0.1-5624](https://github.com/stenzek/duckstation/releases/tag/v0.1-5624) is the last version which will run. But do not expect to recieve any assistance, these builds are no longer supported.

To download:
 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download the Windows x64 build. This is a zip archive containing the prebuilt binary.
 - Alternatively, direct download link: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-release.zip
 - Extract the archive **to a subdirectory**. The archive has no root subdirectory, so extracting to the current directory will drop a bunch of files in your download directory if you do not extract to a subdirectory.

Once downloaded and extracted, you can launch the emulator with `duckstation-qt-x64-ReleaseLTCG.exe`. Follow the Setup Wizard to get started.

**If you get an error about `vcruntime140_1.dll` being missing, you will need to update your Visual C++ runtime.** You can do that from this page: https://support.microsoft.com/en-au/help/2977003/the-latest-supported-visual-c-downloads. Specifically, you want the x64 runtime, which can be downloaded from https://aka.ms/vs/17/release/vc_redist.x64.exe.

### Linux

The only supported version of DuckStation for Linux are the AppImage and Flatpak in the releases page. If you installed DuckStation from another source or distribution (e.g. EmuDeck), you should contact the packager for support, we have no control over it.

#### AppImage

The AppImages require a distribution equivalent to Ubuntu 22.04 or newer to run.

 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download `duckstation-x64.AppImage`.
 - Run `chmod a+x` on the downloaded AppImage -- following this step, the AppImage can be run like a typical executable.

#### Flatpak

 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download `duckstation-x64.flatpak`.
 - Run `flatpak install ./duckstation-x64.flatpak`.

or, if you have FlatHub set up:
 - Run `flatpak install org.duckstation.DuckStation`.

Use `flatpak run org.duckstation.DuckStation` to start, or select `DuckStation` in the launcher of your desktop environment. Follow the Setup Wizard to get started.
 
### macOS

Universal MacOS builds are provided for both x64 and ARM64 (Apple Silicon).

MacOS Big Sir (11.0) is required, as this is also the minimum requirement for Qt.

To download:
 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download `duckstation-mac-release.zip`.
 - Extract the zip by double-clicking it.
 - Open DuckStation.app, optionally moving it to your desired location first.
 - Depending on GateKeeper configuration, you may need to right click -> Open the first time you run it, as code signing certificates are out of the question for a project which brings in zero revenue.
 
### Android

You will need a device with armv7 (32-bit ARM), AArch64 (64-bit ARM), or x86_64 (64-bit x86). 64-bit is preferred, the requirements are higher for 32-bit, you'll probably want at least a 1.5GHz CPU.

Download from Google Play: https://play.google.com/store/apps/details?id=com.github.stenzek.duckstation
APK and Beta Downloads: https://www.duckstation.org/android/

**No support is provided for the Android app**, it is free and your expectations should be in line with that. Please **do not** email me about issues about it, or ask for help, you will be ignored.

To use:
1. Install and run the app for the first time.
2. Follow the setup wizard.

If you have an external controller, you will need to map the buttons and sticks in settings.

### LibCrypt protection and SBI files

A number of PAL region games use LibCrypt protection, requiring additional CD subchannel information to run properly. libcrypt not functioning usually manifests as hanging or crashing, but can sometimes affect gameplay too, depending on how the game implemented it.

For these games, make sure that the CD image and its corresponding SBI (.sbi) file have the same name and are placed in the same directory. DuckStation will automatically load the SBI file when it is found next to the CD image.

For example, if your disc image was named `Spyro3.cue`, you would place the SBI file in the same directory, and name it `Spyro3.sbi`.

CHD images with built-in subchannel information are also supported.

## Building

### Windows
Requirements:
 - Visual Studio 2022


1. Clone the respository: `git clone https://github.com/stenzek/duckstation.git`.
2. Download the dependencies pack from https://github.com/stenzek/duckstation-ext-qt-minimal/releases/download/latest/deps-x64.7z, and extract it to `dep\msvc`.
3. Open the Visual Studio solution `duckstation.sln` in the root, or "Open Folder" for cmake build.
4. Build solution.
5. Binaries are located in `bin/x64`.
6. Run `duckstation-qt-x64-Release.exe` or whichever config you used.

### Linux
Requirements (Debian/Ubuntu package names):
 - CMake (`cmake`)
 - SDL2 (at least version 2.30.2) (`libsdl2-dev` `libxrandr-dev`)
 - pkgconfig (`pkg-config`)
 - Qt 6 (at least version 6.7.0) (`qt6-base-dev` `qt6-base-private-dev` `qt6-base-dev-tools` `qt6-tools-dev` `libqt6svg6`)
 - git (`git`) (Note: needed to clone the repository and at build time)
 - When Wayland is enabled (default): (`libwayland-dev` `libwayland-egl-backend-dev` `extra-cmake-modules` `qt6-wayland`)
 - libcurl (`libcurl4-openssl-dev`)
 - Optional for faster building: Ninja (`ninja-build`)

Due to Ubuntu LTS using older package versions, DuckStation releases use manually built dependency libraries. If you are using a distribution with older package versions, you can build the required library versions using the script found at https://github.com/stenzek/duckstation/blob/master/scripts/build-dependencies.sh.

Fedora package names (assumes you are using the dependency build script):
```
alsa-lib-devel brotli-devel clang cmake dbus-devel egl-wayland-devel fontconfig-devel gcc-c++ libcurl-devel libdecor-devel libevdev-devel libICE-devel libinput-devel libSM-devel libX11-devel libXau-devel libxcb-devel libXcomposite-devel libXcursor-devel libXext-devel libXfixes-devel libXft-devel libXi-devel libxkbcommon-devel libxkbcommon-x11-devel libXpresent-devel libXrandr-devel libXrender-devel lld llvm make mesa-libEGL-devel mesa-libGL-devel ninja-build openssl-devel patch pcre2-devel perl-Digest-SHA pipewire-devel pulseaudio-libs-devel systemd-devel wayland-devel xcb-util-cursor-devel xcb-util-devel xcb-util-errors-devel xcb-util-image-devel xcb-util-keysyms-devel xcb-util-renderutil-devel xcb-util-wm-devel xcb-util-xrm-devel zlib-devel
```

1. Clone the repository: `git clone https://github.com/stenzek/duckstation.git -b dev`.
2. Create a build directory, either in-tree or elsewhere.
3. Run CMake to configure the build system. Assuming a build subdirectory of `build-release`, run `cmake -Bbuild-release -DCMAKE_BUILD_TYPE=Release`. If you have installed Ninja, add `-GNinja` at the end of the CMake command line for faster builds. If you used the dependency build script, the path you built the dependencies in should be supplied with `-DCMAKE_PREFIX_PATH=/path/to/deps`.
4. Compile the source code. For the example above, run `cmake --build build-release --parallel`.
5. Run the binary, located in the build directory under `bin/duckstation-qt`.

### macOS

Requirements:
 - CMake
 - Xcode


1. Clone the repository: `git clone https://github.com/stenzek/duckstation.git`.
2. Build the dependencies. `scripts/build-dependencies-mac.sh /PATH/TO/SAVE/DEPS` (substitute with your own location).
2. Run CMake to configure the build system: `cmake -Bbuild-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/PATH/TO/SAVE/DEPS`. 
4. Compile the source code: `cmake --build build-release --parallel`.
5. Run the binary, located in the build directory under `bin/DuckStation.app`.

## User Directories
The "User Directory" is where you should place your BIOS images, where settings are saved to, and memory cards/save states are saved by default.
An optional [SDL game controller database file](#sdl-game-controller-database) can be also placed here.

This is located in the following places depending on the platform you're using:

- Windows: My Documents\DuckStation
- Linux: `$XDG_DATA_HOME/duckstation`, or `~/.local/share/duckstation`.
- macOS: `~/Library/Application Support/DuckStation`.

So, if you were using Linux, you would place your BIOS images in `~/.local/share/duckstation/bios`. This directory will be created upon running DuckStation
for the first time.

If you wish to use a "portable" build, where the user directory is the same as where the executable is located, create an empty file named `portable.txt`
in the same directory as the DuckStation executable.

## Bindings for Qt frontend
Your keyboard or game controller can be used to simulate a variety of PlayStation controllers. Controller input is supported through DInput, XInput, and SDL backends and can be changed through `Settings -> General Settings`.

To bind your input device, go to `Settings -> Controllers`. Each of the buttons/axes for the simulated controller will be listed, alongside the corresponding key/button on your device that it is currently bound to. To rebind, click the box next to the button/axis name, and press the key or button on your input device that you wish to bind to. When binding rumble, simply press any button on the controller you wish to send rumble to.

## SDL Game Controller Database
DuckStation releases ship with a database of game controller mappings for the SDL controller backend, courtesy of https://github.com/gabomdq/SDL_GameControllerDB. The included `gamecontrollerdb.txt` file can be found in the `database` subdirectory of the DuckStation program directory.

If you are experiencing issues binding your controller with the SDL controller backend, you may need to add a custom mapping to the database file. Make a copy of `gamecontrollerdb.txt` and place it in your [user directory](#user-directories) (or directly in the program directory, if running in portable mode) and then follow the instructions in the [SDL_GameControllerDB repository](https://github.com/gabomdq/SDL_GameControllerDB) for creating a new mapping. Add this mapping to the new copy of `gamecontrollerdb.txt` and your controller should then be recognized properly.

## Default bindings
Controller 1:
 - **Left Stick:** W/A/S/D
 - **Right Stick:** T/F/G/H
 - **D-Pad:** Up/Left/Down/Right
 - **Triangle/Square/Circle/Cross:** I/J/L/K
 - **L1/R1:** Q/E
 - **L2/R2:** 1/3
 - **L3/R3:** 2/4
 - **Start:** Enter
 - **Select:** Backspace

Hotkeys:
 - **Escape:** Open Pause Menu
 - **F11:** Toggle Fullscreen
 - **Tab:** Temporarily Disable Speed Limiter
 - **Space:** Pause/Resume Emulation

## Disclaimers

Icon by icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

"PlayStation" and "PSX" are registered trademarks of Sony Interactive Entertainment Europe Limited. This project is not affiliated in any way with Sony Interactive Entertainment.
