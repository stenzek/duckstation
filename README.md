# DuckStation - PlayStation 1, aka. PSX Emulator
[Latest News](#latest-news) | [Features](#features) | [Screenshots](#screenshots) | [Downloading and Running](#downloading-and-running) | [Libretro Core](#libretro-core) | [Building](#building) | [Disclaimers](#disclaimers)

**Discord Server:** https://discord.gg/Buktv3t

**Latest Builds for Windows and Linux (AppImage)** https://github.com/stenzek/duckstation/releases/tag/latest

**Game Compatibility List:** https://docs.google.com/spreadsheets/d/1H66MxViRjjE5f8hOl5RQmF5woS1murio2dsLn14kEqo/edit

**Wiki:** https://www.duckstation.org/wiki/

DuckStation is an simulator/emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability. The goal is to be as accurate as possible while maintaining performance suitable for low-end devices. "Hack" options are discouraged, the default configuration should support all playable games with only some of the enhancements having compatibility issues.

A "BIOS" ROM image is required to to start the emulator and to play games. You can use an image from any hardware version or region, although mismatching game regions and BIOS regions may have compatibility issues. A ROM image is not provided with the emulator for legal reasons, you should dump this from your own console using Caetla or other means.

## Latest News
Older entries are available at https://github.com/stenzek/duckstation/blob/master/NEWS.md

- 2021/07/25: Ability to boot games directly from CD-ROM added. You may want to reduce the readahead size to reduce hitches on seek/loading.
- 2021/07/11: UWP/Xbox one port added. Follow the instructions in "Universal Windows Platform / Xbox One" below.
- 2021/07/10: Direct3D 12 hardware renderer added. It does not support downsampling or postprocessing (was mainly intended for Xbox).
- 2021/06/25: Ability to undelete files from memory card editor added.
- 2021/06/22: Measured achievements for RetroAchievements added.
- 2021/06/19: Leaderboards for RetroAchievements added.
- 2021/06/01: Auto loading/applying of PPF patches added.
- 2021/05/23: Save RAM (srm) support added to libretro core.
- 2021/05/23: CD-ROM seek speedup enhancement added.
- 2021/05/16: Auto fire (toggle pressing) buttons added.

## Features

DuckStation features a fully-featured frontend built using Qt, as well as a fullscreen/TV UI based on Dear ImGui. An Android version has been started, but is not yet feature complete.

<p align="center">
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main-qt.png" alt="Main Window Screenshot" />
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/bigduck.png" alt="Fullscreen UI Screenshot" />
</p>

Other features include:

 - CPU Recompiler/JIT (x86-64, armv7/AArch32 and AArch64)
 - Hardware (D3D11, D3D12, OpenGL, Vulkan) and software rendering
 - Upscaling, texture filtering, and true colour (24-bit) in hardware renderers
 - PGXP for geometry precision, texture correction, and depth buffer emulation
 - Adaptive downsampling filter
 - Post processing shader chains
 - "Fast boot" for skipping BIOS splash/intro
 - Save state support
 - Windows, Linux, **highly experimental** macOS support
 - Supports bin/cue images, raw bin/img files, MAME CHD, single-track ECM, MDS/MDF, and unencrypted PBP formats.
 - Direct booting of homebrew executables
 - Direct loading of Portable Sound Format (psf) files
 - Digital and analog controllers for input (rumble is forwarded to host)
 - Namco GunCon lightgun support (simulated with mouse)
 - NeGcon support
 - Qt and NoGUI frontends for desktop
 - Automatic updates for Windows builds
 - Automatic content scanning - game titles/hashes are provided by redump.org
 - Optional automatic switching of memory cards for each game
 - Supports loading cheats from libretro or PCSXR format lists
 - Memory card editor and save importer
 - Emulated CPU overclocking
 - Integrated and remote debugging
 - Multitap controllers (up to 8 devices)
 - RetroAchievements
 - Automatic loading/applying of PPF patches

## System Requirements
 - A CPU faster than a potato. But it needs to be x86_64, AArch32/armv7, or AArch64/ARMv8, otherwise you won't get a recompiler and it'll be slow.
 - For the hardware renderers, a GPU capable of OpenGL 3.1/OpenGL ES 3.0/Direct3D 11 Feature Level 10.0 (or Vulkan 1.0) and above. So, basically anything made in the last 10 years or so.
 - SDL, XInput or DInput compatible game controller (e.g. XB360/XBOne). DualShock 3 users on Windows will need to install the official DualShock 3 drivers included as part of PlayStation Now.

## Downloading and running
Binaries of DuckStation for Windows x64/ARM64, Linux x86_64 (in AppImage format), and Android ARMv7/ARMv8 are available via GitHub Releases and are automatically built with every commit/push. Binaries or packages distributed through other sources may be out of date and are not supported by the developer, please speak to them for support, not us.

### Windows

**Windows 10 is the only version of Windows supported by the developer.** Windows 7/8 may work, but is not supported. I am aware some users are still using Windows 7, but it is no longer supported by Microsoft and too much effort to get running on modern hardware. Game bugs are unlikely to be affected by the operating system, however performance issues should be verified on Windows 10 before reporting.

To download:
 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download the Windows x64 build. This is a zip archive containing the prebuilt binary.
 - Alternatively, direct download link: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-release.zip
 - Extract the archive **to a subdirectory**. The archive has no root subdirectory, so extracting to the current directory will drop a bunch of files in your download directory if you do not extract to a subdirectory.

Once downloaded and extracted, you can launch the emulator with `duckstation-qt-x64-ReleaseLTCG.exe`.
To set up:
1. Either configure the path to a BIOS image in the settings, or copy one or more PlayStation BIOS images to the bios/ subdirectory. On Windows, by default this will be located in `C:\Users\YOUR_USERNAME\Documents\DuckStation\bios`. If you don't want to use the Documents directory to save the BIOS/memory cards/etc, you can use portable mode. See [User directory](#user-directories).
2. If using the Qt frontend, add the directories containing your disc images by clicking `Settings->Add Game Directory`.
2. Select a game from the list, or open a disc image file and enjoy.

**If you get an error about `vcruntime140_1.dll` being missing, you will need to update your Visual C++ runtime.** You can do that from this page: https://support.microsoft.com/en-au/help/2977003/the-latest-supported-visual-c-downloads. Specifically, you want the x64 runtime, which can be downloaded from https://aka.ms/vs/16/release/vc_redist.x64.exe.

**Windows 7 users, TLS 1.2 is not supported by default and you will not be able to use the automatic updater or RetroAchievements.** This knowledge base article contains instructions for enabling TLS 1.1/1.2: https://support.microsoft.com/en-us/topic/update-to-enable-tls-1-1-and-tls-1-2-as-default-secure-protocols-in-winhttp-in-windows-c4bd73d2-31d7-761e-0178-11268bb10392

The Qt frontend includes an automatic update checker. Builds downloaded after 2020/08/07 will automatically check for updates each time the emulator starts, this can be disabled in Settings. Alternatively, you can force an update check by clicking `Help->Check for Updates`.

### Universal Windows Platform / Xbox One

The DuckStation fullscreen UI is available for the Universal Windows Platform and Xbox One.

To use on Xbox One:

1. Ensure your console is in developer mode. You will need to purchase a developer license from Microsoft.
2. Download the duckstation-uwp.appx file.
3. Navigate to the device portal for your console (displayed in the home screen).
4. Install the appx file by clicking Add in the main page.
5. Set the app to Game mode instead of App mode: Scroll down to DuckStation in the listinng, press the `Change View` button, select `View Details`, and change `App` to `Game`.
6. Upload a BIOS image to the local state directory for DuckStation, or place your BIOS image on a removable USB drive. If using a USB drive, you will need to set the BIOS path in DuckStation's settings to point to this directory.
7. Add games to the local state games directory, or use a removable USB drive. Again, you will have to register this path in Game List Settings for it to scan.
8. Launch the app, and enjoy. By default, the `Change View` button will open the quick menu.
9. Don't forget to enable enhancements, an Xbox One S can do 8x resolution scale with 4K output, Series consoles can go higher.

**NOTE:** I'd recommend using a USB drive for saving memory cards, as the local state directory will be removed when you uninstall the app.

### Linux

DuckStation does support Linux, but no support will be provided by the developer due to the huge range and variance of distributions. AppImage builds are provided, but we are not obliged to provide any assistance or investigate any issues, i.e. use at your own risk. However, these binaries may be incompatible with older Linux distros (e.g. Ubuntu distros earlier than 20.04 LTS) due to older distros not providing newer versions of the C/C++ standard libraries required by the AppImage binaries. If you are using a packaged version of DuckStation from another source, please do not ask us for assistance and speak to your packager instead.

#### Binaries

**Linux users are encouraged to build from source when possible and optionally create their own AppImages for features such as desktop integration if desired.**

To download:
 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download either `duckstation-qt-x64.AppImage` or `duckstation-nogui-x64.AppImage` for your desired frontend.
 - Run `chmod a+x` on the downloaded AppImage -- following this step, the AppImage can be run like a typical executable.
 - Optionally use a program such as [appimaged](https://github.com/AppImage/appimaged) or [AppImageLauncher](https://github.com/TheAssassin/AppImageLauncher) for desktop integration. [AppImageUpdate](https://github.com/AppImage/AppImageUpdate) can be used alongside appimaged to easily update your DuckStation AppImage.

### macOS

MacOS builds are no longer provided, as I cannot support a platform which I do not own hardware for, and I'm not spending $1000+ out of my own pocket for a machine which I have no other use for.

You can still build from [source](#building), but you will have to debug any issues encountered yourself.

If anyone is willing to volunteer to support the platform to ensure users have a good experience, I'm more than happy to re-enable the releases.

### Android

You will need a device with armv7 (32-bit ARM), AArch64 (64-bit ARM), or x86_64 (64-bit x86). 64-bit is preferred, the requirements are higher for 32-bit, you'll probably want at least a 1.5GHz CPU.

Google Play is the preferred distribution mechanism and will result in smaller download sizes: https://play.google.com/store/apps/details?id=com.github.stenzek.duckstation

**No support is provided for the Android app**, it is free and your expectations should be in line with that. Please **do not** email me about issues about it, they will be ignored. This repository should also
not be used to raise issues about the app, as it does not contain the app code, only the desktop versions.

If you must use an APK, download links are:

Download link: https://www.duckstation.org/android/duckstation-android.apk

Preview/beta download link: https://www.duckstation.org/android/duckstation-android-beta.apk

Changelog link: https://www.duckstation.org/android/changelog.txt

To use:
1. Install and run the app for the first time.
2. Add game directories by tapping the add button and selecting a directory. You can add additional directories afterwards by selecting "Edit Game Directories" from the menu.
3. Tap a game to start. When you start a game for the first time it will prompt you to import a BIOS image.

If you have an external controller, you will need to map the buttons and sticks in settings.

### Libretro Core

DuckStation is available as a libretro core, which can be loaded into a frontend such as RetroArch. It supports most features of the full frontend, within the constraints and limitations of being a libretro core.

The libretro core is provided under the terms of the Creative Commons Attribution-NonCommercial-NoDerivatives International License (BY-NC-ND 4.0, https://creativecommons.org/licenses/by-nc-nd/4.0/). COMMERCIAL DISTRIBUTION AND USAGE IS PROHIBITED. By downloading the libretro core, you agree that you will not distribute or utilize it with any paid applications, services, or products. This includes server side use in streaming environments. Put simply, it is free for personal use, but you are not allowed to utilize DuckStation to make money.

The core is maintained by a third party, and is not provided as part of the GitHub release. You can download the core through the RetroArch buildbot/core updater, or from the links below. The changelog is viewable at https://www.duckstation.org/libretro/changelog.txt

- Windows x64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_windows_x64.zip
- Android AArch64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_android_aarch64.zip
- Android armv7 (32-bit): https://www.duckstation.org/libretro/duckstation_libretro_android_armv7.zip
- Linux x64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_linux_x64.zip
- Linux AArch64 (64-bit): https://www.duckstation.org/libretro/duckstation_libretro_linux_aarch64.zip
- Linux armv7 (32-bit): https://www.duckstation.org/libretro/duckstation_libretro_linux_armv7.zip

### Region detection and BIOS images
By default, DuckStation will emulate the region check present in the CD-ROM controller of the console. This means that when the region of the console does not match the disc, it will refuse to boot, giving a "Please insert PlayStation CD-ROM" message. DuckStation supports automatic detection disc regions, and if you set the console region to auto-detect as well, this should never be a problem.

If you wish to use auto-detection, you do not need to change the BIOS path each time you switch regions. Simply place the BIOS images for the other regions in the **same directory** as the configured image. This will probably be in the `bios/` subdirectory. Then set the console region to "Auto-Detect", and everything should work fine. The console/log will tell you if you are missing the image for the disc's region.

Some users have been confused by the "BIOS Path" option, the reason it is a path and not a directory is so that an unknown BIOS revision can be used/tested.

Alternatively, the region checking can be disabled in the console options tab. This is the only way to play unlicensed games or homebrew which does not supply a correct region string on the disc, aside from using fastboot which skips the check entirely.

Mismatching the disc and console regions with the check disabled is supported, but may break games if they are patching the BIOS and expecting specific content.

### LibCrypt protection and SBI files

A number of PAL region games use LibCrypt protection, requiring additional CD subchannel information to run properly. libcrypt not functioning usually manifests as hanging or crashing, but can sometimes affect gameplay too, depending on how the game implemented it.

For these games, make sure that the CD image and its corresponding SBI (.sbi) file have the same name and are placed in the same directory. DuckStation will automatically load the SBI file when it is found next to the CD image.

For example, if your disc image was named `Spyro3.cue`, you would place the SBI file in the same directory, and name it `Spyro3.sbi`.

## Building

### Windows
Requirements:
 - Visual Studio 2019
 
1. Clone the respository with submodules (`git clone --recursive https://github.com/stenzek/duckstation.git -b dev`).
2. Open the Visual Studio solution `duckstation.sln` in the root, or "Open Folder" for cmake build.
3. Build solution.
4. Binaries are located in `bin/x64`.
5. Run `duckstation-qt-x64-Release.exe` or whichever config you used.

### Linux
Requirements (Debian/Ubuntu package names):
 - CMake (`cmake`)
 - SDL2 (`libsdl2-dev`, `libxrandr-dev`)
 - pkgconfig (`pkg-config`)
 - Qt 5 (`qtbase5-dev`, `qtbase5-private-dev`, `qtbase5-dev-tools`, `qttools5-dev`)
 - libevdev (`libevdev-dev`)
 - git (`git`) (Note: needed to clone the repository and at build time)
 - When Wayland is enabled (default): `libwayland-dev` `libwayland-egl-backend-dev` `extra-cmake-modules`
 - Optional for RetroAchievements (on by default): libcurl (`libcurl4-gnutls-dev`)
 - Optional for framebuffer output: DRM/GBM (`libgbm-dev`, `libdrm-dev`)
 - Optional for faster building: Ninja (`ninja-build`)

1. Clone the repository. Submodules aren't necessary, there is only one and it is only used for Windows (`git clone https://github.com/stenzek/duckstation.git -b dev`).
2. Create a build directory, either in-tree or elsewhere.
3. Run cmake to configure the build system. Assuming a build subdirectory of `build-release`, `cd build-release && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ..`.
4. Compile the source code. For the example above, run `ninja`.
5. Run the binary, located in the build directory under `bin/duckstation-qt`.

### macOS
**NOTE:** macOS is highly experimental and not tested by the developer. Use at your own risk, things may be horribly broken.

Requirements:
 - CMake (installed by default? otherwise, `brew install cmake`)
 - SDL2 (`brew install sdl2`)
 - Qt 5 (`brew install qt5`)

1. Clone the repository. Submodules aren't necessary, there is only one and it is only used for Windows (`git clone https://github.com/stenzek/duckstation.git -b dev`).
2. Clone the mac externals repository (for MoltenVK): `git clone https://github.com/stenzek/duckstation-ext-mac.git dep/mac`.
2. Create a build directory, either in-tree or elsewhere, e.g. `mkdir build-release`, `cd build-release`.
3. Run cmake to configure the build system: `cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_NOGUI_FRONTEND=OFF -DBUILD_QT_FRONTEND=ON -DUSE_SDL2=ON -DQt5_DIR=/usr/local/opt/qt@5/lib/cmake/Qt5 ..`. You may need to tweak `Qt5_DIR` depending on your system.
4. Compile the source code: `make`. Use `make -jN` where `N` is the number of CPU cores in your system for a faster build.
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

To bind your input device, go to `Settings -> Controller Settings`. Each of the buttons/axes for the simulated controller will be listed, alongside the corresponding key/button on your device that it is currently bound to. To rebind, click the box next to the button/axis name, and press the key or button on your input device that you wish to bind to. When binding rumble, simply press any button on the controller you wish to send rumble to.

## SDL Game Controller Database
DuckStation releases ship with a database of game controller mappings for the SDL controller backend, courtesy of https://github.com/gabomdq/SDL_GameControllerDB. The included `gamecontrollerdb.txt` file can be found in the `database` subdirectory of the DuckStation program directory.

If you are experiencing issues binding your controller with the SDL controller backend, you may need to add a custom mapping to the database file. Make a copy of `gamecontrollerdb.txt` and place it in your [user directory](#user-directories) (or directly in the program directory, if running in portable mode) and then follow the instructions in the [SDL_GameControllerDB repository](https://github.com/gabomdq/SDL_GameControllerDB) for creating a new mapping. Add this mapping to the new copy of `gamecontrollerdb.txt` and your controller should then be recognized properly.

## Default bindings
Controller 1:
 - **D-Pad:** W/A/S/D
 - **Triangle/Square/Circle/Cross:** Numpad8/Numpad4/Numpad6/Numpad2
 - **L1/R1:** Q/E
 - **L2/R2:** 1/3
 - **Start:** Enter
 - **Select:** Backspace

Hotkeys:
 - **Escape:** Power off console
 - **ALT+ENTER:** Toggle fullscreen
 - **Tab:** Temporarily disable speed limiter
 - **Space:** Pause/resume emulation
 
## Tests
 - Passes amidog's CPU and GTE tests in both interpreter and recompiler modes, partial passing of CPX tests

## Screenshots
<p align="center">
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/monkey.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/monkey.jpg" alt="Monkey Hero" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/rrt4.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/rrt4.jpg" alt="Ridge Racer Type 4" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tr2.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tr2.jpg" alt="Tomb Raider 2" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/quake2.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/quake2.jpg" alt="Quake 2" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc.jpg" alt="Croc" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc2.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/croc2.jpg" alt="Croc 2" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff7.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff7.jpg" alt="Final Fantasy 7" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/mm8.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/mm8.jpg" alt="Mega Man 8" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff8.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff8.jpg" alt="Final Fantasy 8 in Fullscreen UI" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/spyro.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/spyro.jpg" alt="Spyro in Fullscreen UI" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tof.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/tof.jpg" alt="Threads of Fate in Fullscreen UI" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/gamegrid.png"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/gamegrid.png" alt="Game Grid" width="400" /></a>
</p>

## Disclaimers

Icon by icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

"PlayStation" and "PSX" are registered trademarks of Sony Interactive Entertainment Europe Limited. This project is not affiliated in any way with Sony Interactive Entertainment.
