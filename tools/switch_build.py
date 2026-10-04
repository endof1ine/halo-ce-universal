"""Ninja rules for the Nintendo Switch build (``ninja switch``).

The Switch port (port/switch/README.md) runs the game as the Android port
does: an ILP32 AArch64 guest image (tools/guest_build.py) under a native
64-bit host, here a libnx homebrew program (port/switch/host) that maps the
image at its fixed address and serves its calls. This graph builds

- the guest image, build/switch/halo_guest.elf, with clang;
- the host, with devkitA64 and libnx, and from both the homebrew program,
  build/switch/halo.nro, the image in its RomFS.

It needs devkitPro (DEVKITPRO, or --devkitpro) with libnx and switch-mesa,
and a clang with the arm64_32 target: port/switch/docker has both.
"""

import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .guest_build import GuestPort, fetch_musl, generate_guest_image, guest_configure_inputs
from .embed_assets import hud_configure_inputs
from .ninja_syntax import Writer

PORT_DIR = Path("port/switch")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/switch")
THIRD_PARTY = BUILD / "third_party"
SDL_TAG = "release-3.4.16"
SDL_DIR = THIRD_PARTY / "SDL3"
SDL_URL = "https://github.com/libsdl-org/SDL.git"

# the Switch guest's defines and processor (tools/guest_build.py)
GUEST_ABI_FLAGS = [
    "-DHALO_SWITCH=1",
    # the guest image of a native host (as Android's): ILP32 AArch64 code,
    # SDL and OpenGL ES through the host
    "-DHALO_GUEST=1",
    "-mcpu=cortex-a57",
]


def _devkitpro(sln: Any) -> Optional[Path]:
    path = getattr(sln, "devkitpro", None) or os.environ.get("DEVKITPRO")
    if path and Path(path).is_dir():
        return Path(path)
    return None


def fetch_third_party() -> Path:
    """Download musl, and SDL3 for its headers only (configure time, once):
    the guest is written against SDL3's API, which the host implements.
    Returns musl's folder."""
    musl_dir = fetch_musl(THIRD_PARTY)
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG} (headers)")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)],
                       check=True)
    return musl_dir


def switch_configure_inputs() -> List[Path]:
    return [Path(__file__), *guest_configure_inputs(), PORT_DIR, *hud_configure_inputs()]


def generate_switch_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file():
        return
    devkitpro = _devkitpro(sln)
    if not devkitpro:
        n.comment("Switch build: no devkitPro found (set DEVKITPRO or pass --devkitpro; port/switch/docker)")
        return
    portlibs = devkitpro / "portlibs" / "switch"
    if not (portlibs / "include" / "GLES3" / "gl32.h").is_file():
        n.comment("Switch build: devkitPro has no switch-mesa (dkp-pacman -S switch-mesa)")
        return
    try:
        musl_dir = fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Switch build disabled: cannot fetch musl/SDL3 ({error})", file=sys.stderr)
        return
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    guest_cc = getattr(sln, "switch_guest_cc", None) or "clang"

    n.comment("Switch build (ninja switch); see port/switch/README.md")
    n.variable("switch_guest_cc", guest_cc)

    guest = generate_guest_image(n, sln, GuestPort(
        prefix="switch",
        build=BUILD,
        abi_flags=GUEST_ABI_FLAGS,
        gl_include=portlibs / "include",
        assembler_target="aarch64-none-elf",
        ar="llvm-ar",
        ld="ld.lld",
        # the AArch64 builtins of compiler-rt (64-bit, which the few the
        # guest uses do not mind: they take no pointers or longs)
        builtins=f"$$({guest_cc} --target=aarch64-linux-gnu --rtlib=compiler-rt -print-libgcc-file-name)",
        sdl_dir=SDL_DIR,
        musl_dir=musl_dir,
    ), config, [Path("port/android/host_imports.list")])

    n.build(outputs="switch_guest", rule="phony", inputs=guest.image)
    if not (PORT_DIR / "host").is_dir():
        n.build(outputs="switch", rule="phony", inputs=guest.image)
    n.newline()
