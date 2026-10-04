"""The Switch build's ninja rules (tools/switch_build.py, tools/guest_build.py).

These run without devkitPro or a network: the third-party downloads are
replaced by empty folders, and devkitPro by a folder with the one header the
generator looks for. They check what a change to the shared guest rules
could break: the Switch guest's defines and code generation, the host's,
the program, and the Android guest's defines next to the Switch's.

    python -m pytest -q tools/test_switch_build.py
"""

import io
import re
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools import android_build, guest_build, switch_build
from tools.ninja_syntax import Writer

ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture(autouse=True)
def in_root(monkeypatch):
    monkeypatch.chdir(ROOT)


def settings(**overrides):
    values = dict(build_dir=Path("build"), linux_cc=None, compiler_launcher=None, port_release=False,
                  port_lto="off", port_portable=True, port_gles=False, port_pgo="off", port_pgo_profile=None,
                  android_ndk=None, android_guest_cc=None, devkitpro=None, switch_guest_cc=None)
    values.update(overrides)
    return SimpleNamespace(**values)


def fake_musl(tmp_path):
    musl = tmp_path / "musl-1.2.5"
    for directory in guest_build.MUSL_DIRECTORIES:
        (musl / "src" / directory).mkdir(parents=True, exist_ok=True)
    (musl / "src" / "thread").mkdir(parents=True, exist_ok=True)
    return musl


def fake_devkitpro(tmp_path):
    devkitpro = tmp_path / "devkitpro"
    header = devkitpro / "portlibs" / "switch" / "include" / "GLES3" / "gl32.h"
    header.parent.mkdir(parents=True)
    header.write_text("")
    return devkitpro


def switch_ninja(tmp_path, monkeypatch, **overrides):
    musl = fake_musl(tmp_path)
    monkeypatch.setattr(switch_build, "fetch_third_party", lambda: musl)
    out = io.StringIO()
    switch_build.generate_switch_build(Writer(out), settings(devkitpro=str(fake_devkitpro(tmp_path)), **overrides))
    return unwrapped(out.getvalue())


def unwrapped(text):
    """the ninja file with each statement on one line (ninja_syntax wraps
    long ones with $ and indentation)"""
    return re.sub(r" *\$\n\s*", " ", text)


def statement_lines(text):
    return unwrapped(text).splitlines()


def test_switch_build_has_the_program(tmp_path, monkeypatch):
    text = switch_ninja(tmp_path, monkeypatch)
    assert "\nbuild switch: phony build/switch/halo.nro" in text
    assert "build build/switch/halo_guest.elf: switch_guest_link" in text
    assert "build build/switch/halo.nro: switch_nro build/switch/halo.elf" in text


def test_switch_guest_is_the_ilp32_guest_with_its_defines(tmp_path, monkeypatch):
    lines = [line for line in statement_lines(switch_ninja(tmp_path, monkeypatch))
             if "--target=arm64_32-apple-watchos" in line]
    assert lines, "no guest compile flags"
    for line in lines:
        flags = line.split()
        assert "-DHALO_SWITCH=1" in flags and "-DHALO_GUEST=1" in flags
        assert "-DHALO_ANDROID=1" not in flags
        assert "-mcpu=cortex-a57" in flags
        # (the game's floating-point results as on x86)
        assert "-ffp-contract=off" in flags


def test_switch_release_reaches_the_guest_and_the_host(tmp_path, monkeypatch):
    lines = statement_lines(switch_ninja(tmp_path, monkeypatch, port_release=True))
    guest = [line for line in lines if "--target=arm64_32-apple-watchos" in line]
    host = [line for line in lines if "-D__SWITCH__" in line]
    assert guest and all("-DHALO_RELEASE" in line.split() for line in guest)
    assert host and all("-DHALO_RELEASE" in line.split() for line in host)


def test_switch_host_builds_the_shared_posix_code(tmp_path, monkeypatch):
    text = switch_ninja(tmp_path, monkeypatch)
    for source in ("port/linux/src/posix_files.c", "port/linux/src/posix_net.c", "port/switch/host/host_main.c",
                   "build/switch/host/host_import_table.c"):
        assert f"switch_host_cc {source}" in text, source
    assert "-lEGL -lglapi -ldrm_nouveau -lnx" in text


def test_switch_imports_are_the_android_hosts(tmp_path, monkeypatch):
    # one contract for both hosts (port/android/host_imports.list)
    text = switch_ninja(tmp_path, monkeypatch)
    assert "switch_imports port/android/host_imports.list" in text


def test_no_devkitpro_no_switch_build(tmp_path, monkeypatch):
    monkeypatch.delenv("DEVKITPRO", raising=False)
    out = io.StringIO()
    switch_build.generate_switch_build(Writer(out), settings(devkitpro=str(tmp_path / "missing")))
    assert "no devkitPro" in out.getvalue()
    assert "build switch:" not in out.getvalue()


def test_android_guest_keeps_its_defines(tmp_path, monkeypatch):
    musl = fake_musl(tmp_path)
    ndk = tmp_path / "ndk"
    (ndk / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "sysroot" / "usr" / "include").mkdir(parents=True)
    monkeypatch.setattr(android_build, "fetch_third_party", lambda: None)
    monkeypatch.setattr(android_build, "MUSL_DIR", musl)
    out = io.StringIO()
    android_build.generate_android_build(Writer(out), settings(android_ndk=str(ndk)))
    lines = [line for line in statement_lines(out.getvalue()) if "--target=arm64_32-apple-watchos" in line]
    assert lines
    for line in lines:
        flags = line.split()
        assert "-DHALO_ANDROID=1" in flags and "-DHALO_GUEST=1" in flags
        assert "-DHALO_SWITCH=1" not in flags
        assert "-mcpu=cortex-a53" in flags


def test_guest_syscall_table_has_the_64_bit_lseek():
    # musl's lseek takes its 64-bit path only with SYS__llseek, which both
    # hosts serve (65536: host_syscall.c)
    table = (ROOT / "port/android/guest/libc/arch/arm64_32/bits/syscall.h.in").read_text()
    assert "#define __NR__llseek\t65536" in table
    assert "case 65536" in (ROOT / "port/android/host/host_syscall.c").read_text()
    assert "GUEST_SYS_llseek 65536" in (ROOT / "port/switch/host/host_syscall.c").read_text()
