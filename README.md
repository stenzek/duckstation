# DuckStation - PlayStation 1, aka. PSX Emulator
[Latest News](#latest-news) | [Features](#features) | [Screenshots](#screenshots) | [Downloading and Running](#downloading-and-running) | [Libretro Core](#libretro-core) | [Building](#building) | [Disclaimers](#disclaimers)

**Discord Server:** https://discord.gg/Buktv3t

**Latest Windows, Linux (AppImage), and Libretro Builds:** https://github.com/stenzek/duckstation/releases/tag/latest

**Game Compatibility List:** https://docs.google.com/spreadsheets/d/1H66MxViRjjE5f8hOl5RQmF5woS1murio2dsLn14kEqo/edit?usp=sharing

DuckStation is an simulator/emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability. Accuracy is not the main focus of the emulator, but the goal is to be as accurate as possible while maintaining performance suitable for low-end devices. "Hack" options are discouraged, the default configuration should support all playable games with only some of the enhancements having compatibility issues.

A "BIOS" ROM image is required to to start the emulator and to play games. You can use an image from any hardware version or region, although mismatching game regions and BIOS regions may have compatibility issues. A ROM image is not provided with the emulator for legal reasons, you should dump this from your own console using Caetla or other means.

## Latest News

- 2020/09/25: Cheat support added for libretro core.
- 2020/09/23: Game covers added to Qt frontend (see [Adding Game Covers](https://github.com/stenzek/duckstation/wiki/Adding-Game-Covers)).
- 2020/09/19: Memory card importer/editor added to Qt frontend.
- 2020/09/13: Support for chaining post processing shaders added.
- 2020/09/12: Additional texture filtering options added.
- 2020/09/09: Basic cheat support added. Not all instructions/commands are supported yet.
- 2020/09/01: Many additional user settings available, including memory cards and enhancements. Now you can set these per-game.
- 2020/08/25: Automated builds for macOS now available.
- 2020/08/22: XInput controller backend added.
- 2020/08/20: Per-game setting overrides added. Mostly for compatibility, but some options are customizable.
- 2020/08/19: CPU PGXP mode added. It is very slow and incompatible with the recompiler, only use for games which need it.
- 2020/08/15: Playlist support/single memcard for multi-disc games in Qt frontend added.
- 2020/08/07: Automatic updater for standalone Windows builds.
- 2020/08/01: Initial PGXP (geometry/perspective correction) support.
- 2020/07/28: Qt frontend supports displaying interface in multiple languages.
- 2020/07/23: m3u multi-disc support for libretro core.
- 2020/07/22: Support multiple bindings for each controller button/axis.
- 2020/07/18: Widescreen hack enhancement added.
- 2020/07/04: Vulkan renderer now available in libretro core.
- 2020/07/02: Now available as a libretro core.
- 2020/07/01: Lightgun support with custom crosshairs.
- 2020/06/19: Vulkan hardware renderer added.

## Features

DuckStation features a fully-featured frontend built using Qt (pictured), as well as a simplified frontend based on SDL and Dear ImGui. An Android version has been started, but is not yet feature complete.

<p align="center">
  <img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main-qt.png" alt="Main Window Screenshot" />
</p>

Other features include:

 - CPU Recompiler/JIT (x86-64 and AArch64)
 - Hardware (D3D11, OpenGL, Vulkan) and software rendering
 - Upscaling, texture filtering, and true colour (24-bit) in hardware renderers
 - PGXP for geometry precision and texture correction
 - Post processing shader chains
 - "Fast boot" for skipping BIOS splash/intro
 - Save state support
 - Windows, Linux, **highly experimental** macOS support
 - Supports bin/cue images, raw bin/img files, and MAME CHD formats.
 - Direct booting of homebrew executables
 - Direct loading of Portable Sound Format (psf) files
 - Digital and analog controllers for input (rumble is forwarded to host)
 - Namco GunCon lightgun support (simulated with mouse)
 - NeGcon support
 - Qt and SDL frontends for desktop
 - libretro core for Windows and Linux
 - Automatic updates for Windows builds
 - Automatic content scanning - game titles/regions are provided by redump.org
 - Optional automatic switching of memory cards for each game
 - Supports loading cheats from libretro or PCSXR format lists
 - Memory card editor and save importer

## System Requirements
 - A CPU faster than a potato. But it needs to be 64-bit (either x86_64 or AArch64/ARMv8) otherwise you won't get a recompiler and it'll be slow. There are no plans to add any 32-bit recompilers.
 - For the hardware renderers, a GPU capable of OpenGL 3.0/OpenGL ES 3.0/Direct3D 11 Feature Level 10.0 (or Vulkan 1.0) and above. So, basically anything made in the last 10 years or so.
 - SDL or XInput compatible game controller (e.g. XB360/XBOne). DualShock 3 users on Windows will need to install the official DualShock 3 drivers included as part of PlayStation Now.
   - Optional [SDL game contoller database files](#sdl-game-controller-database) are also supported.

## Downloading and running
Binaries of DuckStation for Windows 64-bit, x86_64 Linux x86_64 (in AppImage format), and Android ARMv8/AArch64 are available via GitHub Releases and are automatically built with every commit/push. Binaries or packages distributed through other sources may be out of date and are not supported by the developer.

### Windows

**Windows 10 is the only version of Windows supported by the developer.** Windows 7/8 may work, but is not supported. I am aware some users are still using Windows 7, but it is no longer supported by Microsoft and too much effort to get running on modern hardware. Game bugs are unlikely to be affected by the operating system, however performance issues should be verified on Windows 10 before reporting.

To download:
 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download the Windows x64 build. This is a zip archive containing the prebuilt binary.
 - Alternatively, direct download link: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-windows-x64-release.zip
 - Extract the archive **to a subdirectory**. The archive has no root subdirectory, so extracting to the current directory will drop a bunch of files in your download directory if you do not extract to a subdirectory.

Once downloaded and extracted, you can launch the Qt frontend from `duckstation-qt-x64-ReleaseLTCG.exe`, or the SDL frontend from `duckstation-sdl-x64-ReleaseLTCG.exe`.
To set up:
1. Either configure the path to a BIOS image in the settings, or copy one or more PlayStation BIOS images to the bios/ subdirectory. On Windows, by default this will be located in `C:\Users\YOUR_USERNAME\Documents\DuckStation\bios`.
2. If using the SDL frontend, add the directories containing your disc images by clicking `Settings->Add Game Directory`.
2. Select a game from the list, or open a disc image file and enjoy.

**If you get an error about `vcruntime140_1.dll` being missing, you will need to update your Visual C++ runtime.** You can do that from this page: https://support.microsoft.com/en-au/help/2977003/the-latest-supported-visual-c-downloads. Specifically, you want the x64 runtime, which can be downloaded from https://aka.ms/vs/16/release/vc_redist.x64.exe.

The Qt frontend includes an automatic update checker. Builds downloaded after 2020/08/07 will automatically check for updates each time the emulator starts, this can be disabled in Settings. Alternatively, you can force an update check by clicking `Help->Check for Updates`.

### Linux

Prebuilt binaries for 64-bit Linux distros are available for download in the AppImage format. However, these binaries may be incompatible with older Linux distros (e.g. Ubuntu distros earlier than 18.04.4 LTS) due to older distros not providing newer versions of the C/C++ standard libraries required by the AppImage binaries.

**Linux users are encouraged to build from source when possible and optionally create their own AppImages for features such as desktop integration if desired.**

To download:
 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download either `duckstation-qt-x64.AppImage` or `duckstation-sdl-x64.AppImage` for your desired frontend. Keep in mind that keyboard/controller bindings are currently not customizable through the SDL frontend and should be customized through the Qt frontend instead.
 - Run `chmod a+x` on the downloaded AppImage -- following this step, the AppImage can be run like a typical executable.
 - Optionally use a program such as [appimaged](https://github.com/AppImage/appimaged) or [AppImageLauncher](https://github.com/TheAssassin/AppImageLauncher) for desktop integration. [AppImageUpdate](https://github.com/AppImage/AppImageUpdate) can be used alongside appimaged to easily update your DuckStation AppImage.

### macOS

To download:
 - Go to https://github.com/stenzek/duckstation/releases/tag/latest, and download the Mac build. This is a zip archive containing the prebuilt binary.
 - Alternatively, direct download link: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-mac-release.zip
 - Extract the zip archive. If you're using Safari, apparently this happens automatically. This will give you DuckStation.app.
 - Right click DuckStation.app, and click Open. As the package is not signed (Mac certificates are expensive), you must do this the first time you open it. Subsequent runs can be done by double-clicking.

macOS support is considered experimental and not actively supported by the developer; the builds are provided here as a courtesy. Please feel free to submit issues, but it may be some time before
they are investigated.

**macOS builds do not support automatic updates yet.** If there is sufficient demand, this may be something I will consider.


### Android

A prebuilt APK is now available for Android. However, please keep in mind that the Android version is not yet feature complete, it is more of a preview of things to come. You will need a device running a 64-bit AArch64 userland (anything made in the last few years).

Download link: https://github.com/stenzek/duckstation/releases/download/latest/duckstation-android-aarch64.apk

The main limitations are:
 - User directory is currently hardcoded to `<external storage path>/duckstation`. This is usually `/storage/emulated/0` or `/sdcard`'. So BIOS files go in `/sdcard/duckstation/bios`.
 - Lack of options in menu when emulator is running.
 - Performance is currently lower than the desktop x86_64 counterpart.

To use:
 - Install and run the app for the first time.
 - This will create `/sdcard/duckstation`. Drop your BIOS files in `/sdcard/duckstation/bios`.
 - Add game directories by hitting the `+` icon and selecting a directory.
 - Tap a game to start.


### Title Information

PlayStation game discs do not contain title information. For game titles, we use the redump.org database cross-referenced with the game's executable code.
This database can be manually downloaded and added as `cache/redump.dat`, or automatically downloaded by going into the `Game List Settings` in the Qt Frontend,
and clicking `Update Redump Database`.

### Region detection and BIOS images
By default, DuckStation will emulate the region check present in the CD-ROM controller of the console. This means that when the region of the console does not match the disc, it will refuse to boot, giving a "Please insert PlayStation CD-ROM" message. DuckStation supports automatic detection disc regions, and if you set the console region to auto-detect as well, this should never be a problem.

If you wish to use auto-detection, you do not need to change the BIOS path each time you switch regions. Simply place the BIOS images for the other regions in the **same directory** as the configured image. This will probably be in the `bios/` subdirectory. Then set the console region to "Auto-Detect", and everything should work fine. The console/log will tell you if you are missing the image for the disc's region.

Some users have been confused by the "BIOS Path" option, the reason it is a path and not a directory is so that an unknown BIOS revision can be used/tested.

Alternatively, the region checking can be disabled in the console options tab. This is the only way to play unlicensed games or homebrew which does not supply a correct region string on the disc, aside from using fastboot which skips the check entirely.

Mismatching the disc and console regions with the check disabled is supported, but may break games if they are patching the BIOS and expecting specific content.

### LibCrypt protection and SBI files

A number of PAL region games use LibCrypt protection, requiring additional CD subchannel information to run properly. For these games, make sure that the CD image and its corresponding SBI (.sbi) file have the same name and are placed in the same directory. DuckStation will automatically load the SBI file when it is found next to the CD image.

## Building

### Windows
Requirements:
 - Visual Studio 2019
 
1. Clone the respository with submodules (`git clone --recursive` or `git clone` and `git submodule update --init`).
2. Open the Visual Studio solution `duckstation.sln` in the root, or "Open Folder" for cmake build.
3. Build solution.
4. Binaries are located in `bin/x64`.
5. Run `duckstation-sdl-x64-Release.exe`/`duckstation-qt-x64-Release.exe` or whichever config you used.

### Linux
Requirements (Debian/Ubuntu package names):
 - CMake (`cmake`)
 - SDL2 (`libsdl2-dev`)
 - GTK2.0 for file selector (`libgtk2.0-dev`)
 - Qt 5 (`qtbase5-dev`, `qtbase5-private-dev`, `qtbase5-dev-tools`, `qttools5-dev`)
 - Optional for faster building: Ninja (`ninja-build`)

1. Clone the repository. Submodules aren't necessary, there is only one and it is only used for Windows.
2. Create a build directory, either in-tree or elsewhere.
3. Run cmake to configure the build system. Assuming a build subdirectory of `build-release`, `cd build-release && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ..`.
4. Compile the source code. For the example above, run `ninja`.
5. Run the binary, located in the build directory under `bin/duckstation-sdl`, or `bin/duckstation-qt`.

### macOS
**NOTE:** macOS is highly experimental and not tested by the developer. Use at your own risk, things may be horribly broken.

Requirements:
 - CMake (installed by default? otherwise, `brew install cmake`)
 - SDL2 (`brew install sdl2`)
 - Qt 5 (`brew install qt5`)

1. Clone the repository. Submodules aren't necessary, there is only one and it is only used for Windows.
2. Clone the mac externals repository (for MoltenVK): `git clone https://github.com/stenzek/duckstation-ext-mac.git dep/mac`.
2. Create a build directory, either in-tree or elsewhere, e.g. `mkdir build-release`, `cd build-release`.
3. Run cmake to configure the build system: `cmake -DCMAKE_BUILD_TYPE=Release -DQt5_DIR=/usr/local/opt/qt/lib/cmake/Qt5 ..`. You may need to tweak `Qt5_DIR` depending on your system.
4. Compile the source code: `make`. Use `make -jN` where `N` is the number of CPU cores in your system for a faster build.
5. Run the binary, located in the build directory under `bin/duckstation-sdl`, or `bin/DuckStation.app` for Qt.

### Android
**NOTE:** The Android frontend is still incomplete, not all functionality is available yet. User directory is hardcoded to `/sdcard/duckstation` for now.

Requirements:
 - Android Studio with the NDK and CMake installed

1. Clone the repository. Submodules aren't necessary, there is only one and it is only used for Windows.
2. Open the project in the `android` directory.
3. Select Build -> Build Bundle(s) / APKs(s) -> Build APK(s).
4. Install APK on device, or use Run menu for attached device.

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
Your keyboard and any SDL-compatible game controller can be used to simulate the PS Controller. To bind keys/controllers to buttons, go to
`Settings -> Port Settings`. Each of the buttons will be listed, along with the corresponding key it is bound to. To re-bind the button to a new key,
click the button next to button name, and press the key/button you want to use within 5 seconds.

**Currently, it is only possible to bind one input to each controller button/axis. Multiple bindings per button are planned for the future.**

## Bindings for SDL frontend
Keyboard bindings in the SDL frontend are currently not customizable in the frontend itself. You should use the Qt frontend to set up your key/controller bindings first.

## SDL Game Controller Database
DuckStation uses the SDL2 GameController API for input handling which requires controller devices to have known input mappings.
SDL2 provides an embedded database of recognised controllers in its own source code, however it is rather small and thus limited in practice.

There is an officially endorsed [community sourced database](https://github.com/gabomdq/SDL_GameControllerDB) that can be used to support a much broader range of game controllers in DuckStation.
If your controller is not recognized by DuckStation but can be found in the community database above, just download a recent copy of the `gamecontrollerdb.txt` database file and place it in your [User directory](#user-directories). Your controller should now be recognized by DuckStation.

Alternatively, you can also create your own custom controller mappings from scratch easily using readily available tools. See the referenced community database repository for more information.

Using a mappings database is specially useful when using non-XInput game controllers with DuckStation.

## Default bindings
Controller 1:
 - **D-Pad:** W/A/S/D
 - **Triangle/Square/Circle/Cross:** Numpad8/Numpad4/Numpad6/Numpad2
 - **L1/R1:** Q/E
 - **L2/L2:** 1/3
 - **Start:** Enter
 - **Select:** Backspace

Hotkeys:
 - **Escape:** Power off console
 - **ALT+ENTER:** Toggle fullscreen
 - **Tab:** Temporarily disable speed limiter
 - **Pause/Break:** Pause/resume emulation
 - **Page Up/Down:** Increase/decrease resolution scale in hardware renderers
 - **End:** Toggle software renderer
 
## Libretro Core

DuckStation is available as a libretro core, which can be loaded into a frontend such as RetroArch. It supports most features of the full frontend, within the constraints and limitations of being a libretro core.

Prebuilt binaries for 64-bit Windows, Linux and Android can be found on the releases page. Direct links:
- 64-bit Windows: https://github.com/stenzek/duckstation/releases/download/latest/duckstation_libretro.dll.zip
- 64-bit Linux: https://github.com/stenzek/duckstation/releases/download/latest/duckstation_libretro_x64.so.zip
- AArch64 Linux: https://github.com/stenzek/duckstation/releases/download/latest/duckstation_libretro_linux_aarch64.so.zip
- AArch64 Android: https://github.com/stenzek/duckstation/releases/download/latest/duckstation_libretro_android_aarch64.so.zip

To use, download and extract, and install the core file in RetroArch or your preferred frontend.

To build on Windows, use cmake using the following commands from a `x64 Native Tools Command Prompt for VS 2019`:
- mkdir build
- cd build
- cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRETRO_CORE=ON ..

You should then have a file named `duckstation_libretro.dll` which can be loaded as a core.

To build on Linux, follow the same instructions as for a normal build, but for cmake use `cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRETRO_CORE=ON ..`. The shared library will be named `duckstation_libretro.so` in the current directory.

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
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff8.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff8.jpg" alt="Final Fantasy 8" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main.png"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/main.png" alt="SDL Frontend" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/spyro.jpg"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/spyro.jpg" alt="Spyro 2" width="400" /></a>
  <a href="https://raw.githubusercontent.com/stenzek/duckstation/md-images/gamegrid.png"><img src="https://raw.githubusercontent.com/stenzek/duckstation/md-images/gamegrid.png" alt="Game Grid" width="400" /></a>
</p>

## Disclaimers

Icon by icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

"PlayStation" and "PSX" are registered trademarks of Sony Interactive Entertainment Europe Limited. This project is not affiliated in any way with Sony Interactive Entertainment.


