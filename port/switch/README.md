# Nintendo Switch

`ninja switch` builds the game for the Nintendo Switch as a homebrew program:
`build/switch/halo.nro`. It runs on a Switch with custom firmware
(Atmosphère) from the Homebrew Menu.

The game shows its graphics with OpenGL ES 3.2 (mesa). It plays sound
through audout. It accepts input from the Joy-Con, attached or detached,
and from the Pro Controller.

## Requirements

- A Switch with Atmosphère and the Homebrew Menu.
- The game data of Halo: Combat Evolved for the Xbox (refer to "Game data").
- To build: Docker. `tools/switch_docker.sh` makes a container with
  devkitPro (devkitA64, libnx, switch-mesa) and clang. You do not need to
  install them on your computer.

## Build

1. Go to the root folder of the repository.
2. Enter `tools/switch_docker.sh`. The result is `build/switch/halo.nro`.

For a release build (the game does not stop at a failed assertion, and is
faster), enter `tools/switch_docker.sh --release`.

The container builds in a copy of the repository on a Docker volume, and
copies the results back to `build/switch`. On macOS this is necessary: the
file system ignores case, and `port/linux/include/StdDef.h` then stands in
for `<stddef.h>`.

## Install

1. Copy `halo.nro` to `/switch/` on the SD card.
2. Copy the game data to `/switch/halo/maps/` on the SD card (refer to
   "Game data").
3. Hold R while you start any game. The Homebrew Menu opens in place of
   the game.
4. Start Halo.

The game needs the memory of a game: from the Album, the Homebrew Menu has
too little memory, and Halo shows a message.

To send the game from a computer, press Y in the Homebrew Menu. Then enter
`SWITCH_IP=<address> tools/switch_docker.sh nxlink build/switch/halo.nro`.
The log of the game (host.log) then also appears on the computer.

## Game data

The game needs the `maps/` folder from an Xbox disc image of the game. All
versions operate.

1. Extract the folder from the disc image on a computer, for example with
   `extract-xiso -x <image>`.
2. Copy `maps/` to `/switch/halo/maps/` on the SD card (approximately
   1.8 GB): with a card reader, with hekate's USB mass storage, or with an
   FTP program on the Switch (ftpd).
3. For the movies (the intro, the menu's attract mode and the credits), also
   copy the disc's `bink/` folder to `/switch/halo/bink/` (approximately
   650 MB). Without it, the game skips the movies. `bink/intro.bik` alone
   (10 MB) gives the start's logos.

| Item | Location on the SD card |
| --- | --- |
| Game data | `/switch/halo/maps` |
| Movies | `/switch/halo/bink` |
| Settings | `/switch/halo/config.toml` |
| Saved games (`z:\` and `u:\`) | `/switch/halo/save` |
| Log of the game | `/switch/halo/debug.txt` |
| Log of the Switch program | `/switch/halo/host.log` |

## Controls

The buttons go by their labels, as the game's prompts show them:

| Switch | Xbox | Function in the game |
| --- | --- | --- |
| left stick, right stick | left stick, right stick | move, look |
| ZR | right trigger | fire |
| ZL | left trigger | throw a grenade |
| A | A | jump, accept |
| B | B | melee, back |
| X | X | action, reload |
| Y | Y | change the weapon |
| L | black | change the grenade |
| R | white | flashlight |
| stick clicks | stick clicks | crouch, zoom |
| + | start | pause menu |
| − | back | |

The first controller (or the attached Joy-Con) is player 1. The other
controllers are players 2 to 4 (split screen).

In handheld mode, tap the menus on the screen to choose an item. Tap the
left or right half of a setting's value to change it.

When you press HOME or the Switch goes to sleep during a game, the game
opens its pause menu.

On player 1's controller, hold these buttons together for one second:

| Buttons | Function |
| --- | --- |
| + and − | The controller setup of the system: choose the controllers of players 2 to 4. Each player needs two sticks: a pair of Joy-Con or a Pro Controller. |
| − and X | The keyboard of the system for internet play. When the Switch hosts a game, it shows the invite link of the game. To join a game, enter its invite link. |

## Settings

Most settings are in the game's menus: Settings, Profiles, then a profile.
Changes take effect when you select OK. The Switch has no Controls Setup:
it sets the keyboard's keys.

| Menu | Switch settings |
| --- | --- |
| Video Setup | Handheld Resolution, Docked Resolution, Dynamic Resolution, 30 FPS Lock |
| Gamepads | Face Buttons |
| Gyro Setup (Mouse Setup on a computer) | Gyro Aiming, Gyro Sensitivity, Invert Horizontal, Invert Vertical, Gyro Aim Assist |

The menus write the settings to `/switch/halo/config.toml`. At the first
start, the game writes the file with the default values. The settings are
the settings of Linux, without the window, the mouse and the paths (refer
to [port/linux/README.md](../linux/README.md#settings)). These settings are
only for the Switch:

| Setting | Function |
| --- | --- |
| `display.render_scale` | The screen's pixels for each of the 480 lines of the game, in handheld mode. `1` (the default) is the Xbox's. `1.5` is the 720 lines of the Switch's screen: sharper, and more work for the GPU. |
| `display.render_scale_docked` | The same, when the Switch is docked. The game then shows 1080 lines. `1.5` (the default) draws 720 lines, `2.25` all 1080. |
| `display.dynamic_resolution` | `true` (the default): while the GPU holds the frame rate back, draw fewer lines (down to 480), and more again when it can. Only below a render scale above `1`. |
| `display.lock_30fps` | `true`: 30 frames a second, steadier than a rate that varies below 60, and lighter on the battery. |
| `display.async_shaders` | `false` (the default): the game stops while each shader compiles. `true`: other cores compile them, where their OpenGL contexts can share the game's objects; switch-mesa's cannot, and the game then compiles them itself. |
| `input.button_positions` | `true`: A, B, X and Y go by their positions, as on an Xbox controller (the bottom button jumps). |
| `input.gyro_aim` | `true`: turn player 1's controller to aim. The right stick also aims. |
| `input.gyro_sensitivity` | How far the view turns when you turn the controller. `1`: the same angle. |
| `input.gyro_invert_x`, `input.gyro_invert_y` | Turn the view in the other direction. |
| `input.gyro_aim_assist` | `true` (the default): the aim assist of the controller operates while you aim with the gyro. |

These settings are not in the file. Add them to find problems:

| Setting | Function |
| --- | --- |
| `debug.profile` | Every 10 seconds, write to host.log where the game spends its time (refer to "Find problems"). On in debug builds, off in release builds. |
| `debug.watch_verify` | `true`: compare all the watched memory every frame, and log the changes that the schedule finds late (refer to "Write tracking"). |
| `debug.no_persistent_buffers` | `true`: map the vertex buffers for each write, as before, instead of once. Try it if geometry flickers or shows garbage. |

## Find problems

- `host.log` has the log of the Switch program: the start, the frames per
  second every 10 seconds, the time to compile shaders and to upload
  textures, and the profile.
- `debug.txt` is the log of the game. Its lines also go to `host.log`.
- If the game stops, Atmosphère writes a crash report to
  `/atmosphere/crash_reports`. At the next start through nxlink, the program
  sends the newest report to the computer. Addresses below `0x100000000`
  are in the game: enter
  `llvm-symbolizer --obj=build/switch/halo_guest.elf <address>`.
- `tools/switch_profile.py build/switch-logs/<log>` sums the profile of a log
  by function (run it in the container: `tools/switch_docker.sh python3
  tools/switch_profile.py <log>`).

## How the port operates

The Switch port runs the game as the Android port does. Refer to "How the
port operates" in [port/android/README.md](../android/README.md#how-the-port-operates):

- The game is ILP32 AArch64 code (32-bit pointers), the same guest image as
  Android's (`tools/guest_build.py`), with `HALO_SWITCH`.
- The host is a libnx program (`port/switch/host`). It serves the same calls
  as the Android host (`port/android/host_imports.list`).

What is different on the Switch:

- **Memory.** A homebrew program can put memory at an address of its choice
  only with `svcMapProcessCodeMemory`. The host maps the Xbox memory
  (`0x80000000`), the guest image (`0x88000000`), the Custom Edition tag
  cache (`0x40440000`) and an arena for the rest (`0x90000000` to 4 GB) in
  this way (`host_memory.c`). The kernel puts its own regions at random
  addresses. If one of them is in these ranges, the game shows a message.
  Start the game again.
- **Threads.** The kernel of the Switch schedules strictly by priority. The
  main thread of the game has core 0. The other threads of the game share
  cores 1 and 2 at the priority where the kernel takes turns between them
  (`host_thread.c`). A guest thread's stack must be below 4 GB, so each
  thread switches to a stack in the arena.
- **System calls.** The host performs the Linux system calls of the guest's
  musl with newlib and libnx, and changes their flags, structures and error
  numbers (`host_syscall.c`).
- **Write tracking.** The renderer keeps copies of textures and vertices in
  the Xbox memory. Linux and Android find the writes to them with page
  faults. A homebrew program on the Switch can continue after a fault only
  if Atmosphère lets user exception handlers return, so the host does not
  use faults. The game's file reads and D3D locks report their writes. A
  thread compares the other watched pages with copies (`host_watch.c`).
- **Shaders.** mesa compiles a shader on the CPU in approximately 40 ms.
  The game records the shaders of each map in
  `/switch/halo/save/shader_warm`, and compiles them again when the map
  loads, at the CPU's boosted clock, for at most 8 seconds: the rest at
  their first use. (Other threads cannot compile them: switch-mesa's EGL
  gives their OpenGL contexts none of the game's objects, so a program they
  make is none to the game's. `display.async_shaders` checks this at the
  start, and stays off.)
- **Visibility tests.** The lens flares read the results of their visibility
  tests two frames late, so the CPU does not wait for the GPU.
- **Movies.** The game's playback code (`bink_playback.c`) calls the Bink
  SDK. On the Switch, `port/linux/src/bink_host.c` gives it those functions:
  the host decodes the movie with FFmpeg (`host_bink.c`) and mixes its sound
  into the game's. The other ports skip the movies (`bink_null.c`).

## Licenses

The Switch program contains FFmpeg (the Bink decoders,
`port/switch/docker/build_ffmpeg.sh`), under the GNU LGPL 2.1. The builds of
GitHub Actions include its license (`ffmpeg-COPYING.LGPLv2.1.txt`). The
source of FFmpeg is at https://ffmpeg.org, and the script gives the version
and the options of the build.

## Limits

- The game cannot extract the game data from a disc image on the Switch.
- When the game exits, the Switch goes back to the HOME menu, not to the
  Homebrew Menu.
