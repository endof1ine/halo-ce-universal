"""Ninja rules for the guest image of the Android and Switch builds.

Both ports (port/android/README.md, port/switch/README.md) run the game as
ILP32 AArch64 code - 32-bit pointers, as the game's data formats require -
under a native 64-bit host. The guest image is the same for both but for its
defines: the game sources, the platform layer shared with the Linux port
(port/linux/src) and the guest runtime (port/android/guest) with a subset of
musl as its C library, all compiled by clang for arm64_32-apple-watchos,
converted to ELF assembly (tools/android_asm_convert.py), assembled for
AArch64 and linked at a fixed address below 4 GB (port/android/guest/guest.ld).

Each port's generator calls generate_guest_image() with its own rule prefix
and build folder, and builds its host around the result.
"""

import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from .linux_build import (LINUX_PROFILE, MUSL_MATH_DIR, XDK_INCLUDE, compile_launcher, game_defines_and_includes,
                          game_sources, musl_math_sources, pgo_mode, pgo_profile, profile_use_flags, xdk_headers)
from .embed_assets import hud_assets_build
from .ninja_syntax import Writer

GUEST_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
# the TOML parser config.toml is read with (port/linux/src/port_config.c)
TOML_DIR = Path("port/third_party/tomlc17")
# the SDL3 whose API the guests are written against
SDL_TAG = "release-3.4.16"
SDL_URL = "https://github.com/libsdl-org/SDL.git"
EXPAT_DIR = Path("port/third_party/expat")
EXPAT_SOURCES = ("xmlparse.c", "xmlrole.c", "xmltok.c")
KCP_DIR = Path("port/third_party/kcp")
MONOCYPHER_DIR = Path("port/third_party/monocypher")
MUSL_VERSION = "1.2.5"
MUSL_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION}.tar.gz"

# The guest ABI: AArch64 code with 32-bit pointers (clang's only such target
# is Apple's arm64_32, whose Mach-O output is converted afterwards). The
# Darwin environment is hidden from the sources; the C library is musl. Each
# port adds its own defines and processor.
GUEST_ABI_FLAGS = [
    "--target=arm64_32-apple-watchos",
    "-U__APPLE__",
    "-U__MACH__",
    "-fno-define-target-os-macros",
    "-D__linux__=1",
    "-D__unix__=1",
    # (here each port's GuestPort.abi_flags: its defines, with HALO_GUEST,
    # and its processor)
    "-nostdinc",
    "-fshort-wchar",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-femulated-tls",
    "-mllvm",
    "-aarch64-neon-syntax=generic",
    # no fused multiply-add: the game was written for x87/SSE arithmetic,
    # and its debug assertions (colours within 0..1, unit vectors) trip on
    # the different rounding of fused operations
    "-ffp-contract=off",
    "-O2",
]

# as the Linux build (tools/linux_build.py), minus what only x86 needs
GUEST_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

MUSL_DIRECTORIES = [
    "conf", "ctype", "dirent", "env", "errno", "exit", "fcntl", "internal",
    "locale", "malloc", "malloc/mallocng", "math", "mman", "multibyte",
    "prng", "sched", "select", "signal", "stat", "stdio", "stdlib", "string",
    "time", "unistd",
]
MUSL_FILES = [
    "thread/__lock.c", "thread/__wait.c", "thread/__timedwait.c",
    "thread/__syscall_cp.c", "thread/vmlock.c", "thread/pthread_self.c",
    "thread/pthread_equal.c", "thread/pthread_once.c",
    "thread/pthread_setcancelstate.c", "thread/pthread_testcancel.c",
    "thread/default_attr.c", "thread/lock_ptc.c", "misc/getauxval.c",
    "linux/sysinfo.c", "misc/basename.c", "misc/dirname.c",
    "misc/realpath.c", "misc/uname.c", "misc/ioctl.c", "misc/getrlimit.c",
    "misc/syscall.c", "network/htonl.c", "network/htons.c", "network/ntohl.c",
    "network/ntohs.c", "network/inet_addr.c", "network/inet_aton.c",
    "network/inet_ntoa.c", "network/inet_pton.c", "network/inet_ntop.c",
]
MUSL_THREAD_PREFIXES = (
    "pthread_attr_", "pthread_cond", "pthread_mutex", "pthread_rwlock",
    "pthread_spin", "sem_",
)
# replaced by the guest runtime (port/android/guest/runtime)
MUSL_EXCLUDE = {
    "env/__stack_chk.c", "env/__init_tls.c", "env/__libc_start_main.c",
    "env/__reset_tls.c", "malloc/oldmalloc", "thread/pthread_create.c",
    # unused, and its compiler barrier is an inline assembly statement
    "string/explicit_bzero.c",
    # a byte at a time: the runtime's compares a word at a time
    "string/memcmp.c",
}
# game files that call variadic functions without a prototype in scope, which
# only works under x86's calling convention (tools/android_abi_check.py)
VARIADIC_PROTOTYPE_FILES = {
    "source/ai/action_uncover.c", "source/ai/ai.c", "source/ai/ai_debug.c",
    "source/bungie_net/common/public_key_crypt.c", "source/camera/editor_flying_camera.c",
    "source/game/cheats.c", "source/game/game_engine.c", "source/game/players.c",
    "source/hs/hs.c", "source/interface/hud_nav_points.c",
    "source/networking/telnet_console.c", "source/rasterizer/xbox/rasterizer_xbox_errors.c",
    "source/render/render.c",
}


@dataclass
class GuestPort:
    """What differs between the ports' guest images."""

    # rule name prefix (android, switch) and its upper-case label
    prefix: str
    # the port's build folder (build/android, build/switch)
    build: Path
    # the port's defines (HALO_GUEST, the guest image of a native host, and
    # the port's own) and processor, within GUEST_ABI_FLAGS
    abi_flags: List[str]
    # the folder with the GLES2, GLES3 and KHR headers the guest compiles
    # against, and that tools/android_gl_stubs.py reads
    gl_include: Path
    # the assembler's target (an ELF AArch64 one)
    assembler_target: str
    # the archiver and linker
    ar: str
    ld: str
    # a shell expression for the AArch64 compiler builtins library
    builtins: str
    # the SDL3 sources, for their headers
    sdl_dir: Path
    # musl's sources (fetched by fetch_musl)
    musl_dir: Path


@dataclass
class GuestImage:
    image: Path
    # the host's table of import names and functions (tools/android_imports.py)
    host_import_table: Path


def fetch_musl(third_party: Path) -> Path:
    """Download musl (configure time, once); its source folder."""
    musl_dir = third_party / f"musl-{MUSL_VERSION}"
    third_party.mkdir(parents=True, exist_ok=True)
    if not musl_dir.is_dir():
        print(f"Downloading {MUSL_URL}")
        archive = third_party / f"musl-{MUSL_VERSION}.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), MUSL_URL], check=True)
        subprocess.run(["tar", "xzf", archive.name], cwd=third_party, check=True)
        archive.unlink()
    return musl_dir


def _musl_sources(musl_dir: Path) -> List[Path]:
    src = musl_dir / "src"
    result = set()
    for directory in MUSL_DIRECTORIES:
        for path in (src / directory).glob("*.c"):
            result.add(path)
    for name in MUSL_FILES:
        result.add(src / name)
    for path in (src / "thread").glob("*.c"):
        if path.name.startswith(MUSL_THREAD_PREFIXES):
            result.add(path)
    sources = []
    for path in sorted(result):
        relative = path.relative_to(src).as_posix()
        if relative in MUSL_EXCLUDE or any(relative.startswith(e + "/") for e in MUSL_EXCLUDE):
            continue
        sources.append(path)
    return sources


def guest_configure_inputs() -> List[Path]:
    return [Path(__file__), GUEST_DIR / "guest" / "runtime", LINUX_DIR / "src"]


def generate_guest_image(n: Writer, sln: Any, port: GuestPort, config: Dict[str, Any],
                         host_imports: List[Path]) -> GuestImage:
    """Emit the rules of a port's guest image; host_imports are the lists of
    the host functions the guest calls, besides its OpenGL ES and posix_*
    stubs."""
    prefix = port.prefix
    label = prefix.upper()
    build = port.build
    musl_dir = port.musl_dir
    guest_cc = f"${prefix}_guest_cc"

    guest_dir = build / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    gl_include = guest_dir / "gl_include"
    arch = GUEST_DIR / "guest" / "libc" / "arch" / "arm64_32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = build / "halo_guest.elf"
    python = "$python"

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name=f"{prefix}_alltypes",
        command=f"sed -f {musl_dir}/tools/mkalltypes.sed $in > $out",
        description=f"{label} MUSL $out",
    )
    n.build(outputs=alltypes, rule=f"{prefix}_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", musl_dir / "include" / "alltypes.h.in"])
    n.rule(
        name=f"{prefix}_syscall_h",
        command="cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description=f"{label} MUSL $out",
    )
    n.build(outputs=syscall_h, rule=f"{prefix}_syscall_h", inputs=arch / "bits" / "syscall.h.in")
    n.rule(
        name=f"{prefix}_version_h",
        command=f"echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description=f"{label} MUSL $out",
    )
    n.build(outputs=version_h, rule=f"{prefix}_version_h")

    # the OpenGL ES headers (C declarations only) for the guest
    gl_stamp = gl_include / "stamp"
    n.rule(
        name=f"{prefix}_gl_include",
        command=(f"mkdir -p {gl_include} && ln -sfn {port.gl_include}/GLES2 {gl_include}/GLES2 && "
                 f"ln -sfn {port.gl_include}/GLES3 {gl_include}/GLES3 && "
                 f"ln -sfn {port.gl_include}/KHR {gl_include}/KHR && touch $out"),
        description=f"{label} GL HEADERS",
    )
    n.build(outputs=gl_stamp, rule=f"{prefix}_gl_include")

    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    n.rule(
        name=f"{prefix}_gl_stubs",
        command=(f"{python} tools/android_gl_stubs.py {LINUX_DIR}/src/gl.h {port.gl_include}/GLES3/gl32.h "
                 f"{port.gl_include}/GLES2/gl2ext.h {guest_gl_c} {gl_imports}"),
        description=f"{label} GL STUBS",
    )
    n.build(outputs=[guest_gl_c, gl_imports], rule=f"{prefix}_gl_stubs",
            implicit=[Path("tools/android_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"])

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name=f"{prefix}_posix_stubs",
        command=f"{python} tools/android_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {posix_imports}",
        description=f"{label} POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule=f"{prefix}_posix_stubs",
            implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    imports_s = gen_dir / "imports.s"
    host_table_c = build / "host" / "host_import_table.c"
    n.rule(
        name=f"{prefix}_imports",
        command=f"{python} tools/android_imports.py --host-table {host_table_c} {imports_s} $in",
        description=f"{label} IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule=f"{prefix}_imports",
            inputs=[*host_imports, posix_imports, gl_imports],
            implicit=[Path("tools/android_imports.py")])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, gl_stamp,
                         semantics_header, platform_semantics_header]

    # ---------- guest compilation: C -> Darwin assembly -> ELF assembly -> object

    n.rule(
        name=f"{prefix}_guest_cc",
        command=(f"{compile_launcher(sln)}{guest_cc} -MMD -MF $out.d $cflags -S $in -o $out.darwin.s && "
                 f"{python} tools/android_asm_convert.py $out.darwin.s $out.s && "
                 f"{guest_cc} --target={port.assembler_target} -c $out.s -o $out"),
        description=f"{label} CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name=f"{prefix}_guest_as",
        command=f"{guest_cc} --target={port.assembler_target} -c $in -o $out",
        description=f"{label} AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {musl_dir}/arch/generic",
        f"-isystem {musl_dir}/include",
    ]
    guest_abi = " ".join(GUEST_ABI_FLAGS[:6] + port.abi_flags + GUEST_ABI_FLAGS[6:]
                         + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = [Path("tools/android_asm_convert.py"), *generated_headers]
    # profile-guided optimisation with the Linux build's profile (committed,
    # or trained by the Linux build with --pgo=train): the game and platform
    # code are the same, and functions that differ simply go without
    guest_cc_binary = getattr(sln, f"{prefix}_guest_cc", None) or "clang"
    profile = pgo_profile(sln, LINUX_PROFILE if pgo_mode(sln) == "train" else None, [LINUX_PROFILE],
                          guest_cc_binary)
    profile_flags = " ".join(profile_use_flags(profile))
    if profile:
        tool_implicit.append(profile)

    def guest_object(source: Path, cflags: str, subfolder: str = "") -> Path:
        obj = obj_dir / subfolder / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(build)):
            obj = obj_dir / subfolder / source.relative_to(build).with_suffix(".o")
        n.build(outputs=obj, rule=f"{prefix}_guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{musl_dir}/arch/generic", f"-I{libc_internal}",
        f"-I{GUEST_DIR}/guest/libc/src_include", f"-I{musl_dir}/src/include",
        f"-I{musl_dir}/src/internal", f"-I{libc_include}", f"-I{musl_dir}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources(musl_dir)]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name=f"{prefix}_ar",
        command=f"rm -f $out && {port.ar} rcs $out @$out.rsp",
        description=f"{label} AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule=f"{prefix}_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags), profile_flags,
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include", game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {GUEST_DIR}/include/halo_android_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))

    # the platform layer shared with Linux, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", profile_flags,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{GUEST_DIR}/guest/runtime",
        f"-I{GUEST_DIR}/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}",
        "-Isource -Isource/cseries",
        f"-I{port.sdl_dir}/include", f"-I{gl_include}", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    guest_host_only = {"memory_watch.c"}  # replaced by guest_memory_watch.c
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        objects.append(guest_object(source, platform_cflags))
    # the high-res HUD's textures (port/assets/hud; port/linux/src/hud_hires.c)
    for source in hud_assets_build(n, prefix, gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    # the settings file's parser (port/third_party/tomlc17)
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    # the menus' XML parser (port/third_party/expat; menu_files.c)
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    # internet play's reliable streams (port/third_party/kcp; p2p.c)
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    # internet play's signatures, for public games' listings
    # (port/third_party/monocypher; p2p_crypto.c)
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags))
    # the game's sin, pow and the rest, the same on every port
    # (port/include/halo_math.h)
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", profile_flags, *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{GUEST_DIR}/guest/runtime", f"-I{GUEST_DIR}/include",
        f"-I{arch}", f"-I{musl_dir}/arch/generic", f"-I{libc_internal}",
        f"-I{GUEST_DIR}/guest/libc/src_include", f"-I{musl_dir}/src/include",
        f"-I{musl_dir}/src/internal", f"-I{libc_include}", f"-I{musl_dir}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{GUEST_DIR}/guest/runtime", f"-I{GUEST_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{port.sdl_dir}/include", f"-I{gl_include}", *libc_includes,
    ])
    runtime_dir = GUEST_DIR / "guest" / "runtime"
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule=f"{prefix}_guest_as", inputs=imports_s)
    objects.append(imports_o)

    # ---------- the guest image

    linker_script = GUEST_DIR / "guest" / "guest.ld"
    n.rule(
        name=f"{prefix}_guest_link",
        command=(f"{port.ld} -m aarch64linux -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc} {port.builtins}"),
        description=f"{label} LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule=f"{prefix}_guest_link", inputs=objects, implicit=[libguestc, linker_script])
    return GuestImage(image=image, host_import_table=host_table_c)
