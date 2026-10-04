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
    _generate_host(n, devkitpro, guest.image, guest.host_import_table, getattr(sln, "port_release", False))
    n.newline()


# the host's code generation (as devkitPro's template's)
HOST_ARCH = "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
HOST_LIBRARIES = ["EGL", "glapi", "drm_nouveau", "nx"]
TOML_DIR = Path("port/third_party/tomlc17")


def _version() -> str:
    try:
        return subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], capture_output=True, text=True,
                              check=True).stdout.strip() or "0"
    except (subprocess.CalledProcessError, OSError):
        return "0"


def _generate_host(n: Writer, devkitpro: Path, image: Path, import_table: Path, release: bool) -> None:
    """The libnx host (port/switch/host) and the program, build/switch/halo.nro."""
    host_dir = BUILD / "host"
    obj_dir = host_dir / "obj"
    include_dir = host_dir / "include"
    romfs = BUILD / "romfs"
    elf = BUILD / "halo.elf"
    nacp = BUILD / "halo.nacp"
    icon = BUILD / "icon.jpg"
    nro = BUILD / "halo.nro"
    tools = devkitpro / "tools" / "bin"
    a64 = devkitpro / "devkitA64" / "bin"

    n.variable("switch_host_cc", str(a64 / "aarch64-none-elf-gcc"))
    n.variable("switch_host_cxx", str(a64 / "aarch64-none-elf-g++"))

    # the guest's system call numbers (Linux's), which the host serves
    guest_syscall_h = include_dir / "guest_syscall.h"
    n.rule(name="switch_copy", command="mkdir -p $$(dirname $out) && cp $in $out", description="SWITCH COPY $out")
    n.build(outputs=guest_syscall_h, rule="switch_copy", inputs=BUILD / "guest" / "libc_include" / "bits" / "syscall.h")

    n.rule(
        name="switch_host_cc",
        command="$switch_host_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="SWITCH HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_cflags = " ".join([
        HOST_ARCH, "-O2", "-g", "-Wall", "-Wno-unused-function", "-ffunction-sections", "-D__SWITCH__",
        *(["-DHALO_RELEASE"] if release else []),
        f"-I{PORT_DIR}/host", f"-I{include_dir}", "-Iport/android/include", f"-I{LINUX_DIR}/src", f"-I{TOML_DIR}",
        f"-I{SDL_DIR}/include", f"-I{devkitpro}/libnx/include", f"-I{devkitpro}/portlibs/switch/include",
    ])
    sources = sorted((PORT_DIR / "host").glob("*.c")) + [
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c", TOML_DIR / "tomlc17.c",
    ]
    objects = []
    for source in sources:
        obj = obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="switch_host_cc", inputs=source, implicit=[guest_syscall_h],
                variables={"cflags": host_cflags + (" -w" if source.parent == TOML_DIR else "")})
        objects.append(obj)
    table_obj = obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule="switch_host_cc", inputs=import_table, variables={"cflags": host_cflags})
    objects.append(table_obj)

    # (with g++: mesa is C++)
    n.rule(
        name="switch_host_link",
        command=(f"$switch_host_cxx -specs={devkitpro}/libnx/switch.specs -g {HOST_ARCH} -Wl,-Map,$out.map "
                 f"-o $out @$out.rsp -L{devkitpro}/portlibs/switch/lib -L{devkitpro}/libnx/lib "
                 + " ".join(f"-l{library}" for library in HOST_LIBRARIES)),
        description="SWITCH HOST LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=elf, rule="switch_host_link", inputs=objects)

    # the program: the host, its metadata and icon, the guest in its RomFS
    n.build(outputs=romfs / "halo_guest.elf", rule="switch_copy", inputs=image)
    n.rule(
        name="switch_nacp",
        command=f"{tools}/nacptool --create \"Halo: Combat Evolved\" \"halo-ce-universal\" \"{_version()}\" $out",
        description="SWITCH NACP $out",
    )
    n.build(outputs=nacp, rule="switch_nacp", implicit=[Path("tools/switch_build.py")])
    n.rule(
        name="switch_icon",
        command="convert $in -resize 256x256 -background black -flatten -quality 90 $out",
        description="SWITCH ICON $out",
    )
    n.build(outputs=icon, rule="switch_icon", inputs=Path("port/android/art/android-icon.png"))
    n.rule(
        name="switch_nro",
        command=f"{tools}/elf2nro $in $out --nacp={nacp} --icon={icon} --romfsdir={romfs} > /dev/null",
        description="SWITCH NRO $out",
    )
    n.build(outputs=nro, rule="switch_nro", inputs=elf, implicit=[nacp, icon, romfs / "halo_guest.elf"])
    n.build(outputs="switch", rule="phony", inputs=nro)
