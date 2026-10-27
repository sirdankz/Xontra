#!/usr/bin/env python3
"""One command from the Contra (US) disassembly to an Xbox disc image.

    python setup.py                 # everything -> out/Release/XISO/Contra-NES-X.iso
    python setup.py --check         # say what is there and what is missing
    python setup.py --skip-build    # stop before compiling (to build in Visual Studio)
    python setup.py --config Debug  # the debug configuration
    python setup.py --no-assemble   # use baserom.nes as it is: no fetching, no assembling

Needs: your own Contra (US) cartridge as baserom.nes in this folder, Python
3.8+ (standard library only), git, the RXDK toolchain (its clang also builds
the PC-side tools; Visual Studio's cl or a gcc on the PATH are used when this
RXDK has no host sysroot) and an internet connection the first time.

The game is not in this repository. It is made here, into folders
.gitignore keeps out:

  1. the cartridge, build/Contra.nes: the annotated disassembly
     (vermiceli/nes-contra-us at DECOMP_REV, fetched into extern/nes-contra-us)
     assembled and linked with cc65's ca65 and ld65 (cc65/cc65 at CC65_REV,
     fetched into extern/cc65 and built for Windows). The disassembly is the
     code; the compressed graphics and the music it does not carry are taken
     from your baserom.nes at the offsets its assets.txt lists. The result is
     checked against the cartridge (the disassembly builds it byte for byte)
  2. tools/hosttest built for Windows (build/hosttest.exe): the port's NES
     core on the PC, including deterministic rollback regression tests.
     Original dashboard icons are generated without reading the cartridge.
  3. data/Contra.nes: the disc's copy, D:\\data\\Contra.nes
  4. rxdk.project.json (gen_manifest.py) and the Visual Studio project
     (gen_vcxproj.py)
  5. the build: Rxdk.Cli compiles, links, bundles the icons and packs the ISO
"""
import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools", "xbox"))
import xbconfig  # noqa: E402

NAME = "Contra-NES-X"
BUILD = os.path.join(ROOT, "build")
DATA = os.path.join(ROOT, "data")
ROM_NAMES = ("baserom.nes", "Contra.nes", "contra.nes", "Contra (USA).nes", "Contra (U).nes")
ROM_OUT = os.path.join(BUILD, "Contra.nes")
# Contra (US), as the disassembly's README and build.ps1 give it: an iNES
# file of 16 header bytes and eight 16 KB PRG banks (UxROM, CHR-RAM)
ROM_SIZE = 16 + 8 * 0x4000
ROM_MD5 = "7bdad8b4a7a56a634c9649d20bd3011b"
ROM_SHA512 = ("1c747c78c678f14a68d4e5fcae065298a103f833638775860f0e5c5ffaa061f6"
              "2d45fd8942148b1507c1fd57fde950a5d83f9f84a9782ec048a56067740c48e9")
# the header the disassembly's ines_header.asm writes; dumps differ in the
# unused bytes 7-15, and only there
INES_HEADER = bytes([0x4E, 0x45, 0x53, 0x1A, 0x08, 0x00, 0x21, 0x00]) + bytes(8)
PROBOTECTOR_MD5 = "7127f616c13b58087481dd9e93aeb2ff"

# the annotated disassembly, and the assembler and linker that build it
DECOMP_URL = "https://github.com/vermiceli/nes-contra-us"
DECOMP_REV = "687d651c021fd7020f10d05b970ccb62663c94bd"
DECOMP = os.path.join(ROOT, "extern", "nes-contra-us")
CC65_URL = "https://github.com/cc65/cc65"
CC65_REV = "555282497c3ecf8b313d87d5973093af19c35bd5"        # V2.19
CC65 = os.path.join(ROOT, "extern", "cc65")
CA65_EXE = os.path.join(BUILD, "ca65.exe")
LD65_EXE = os.path.join(BUILD, "ld65.exe")
# the order build.ps1 assembles and links them in
OBJECTS = ("ram", "constants", "ines_header", "bank0", "bank1", "bank2", "bank3", "bank4",
           "bank5", "bank6", "bank7")

HOSTTEST_EXE = os.path.join(BUILD, "hosttest.exe")


def say(msg):
    print("setup: " + msg, flush=True)


def die(msg):
    print("setup: ERROR: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd, cwd=ROOT, quiet=False):
    r = subprocess.run(cmd, cwd=cwd, capture_output=quiet, text=True, encoding="utf-8",
                       errors="replace")
    if r.returncode != 0:
        if quiet:
            sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        die("failed (%d): %s" % (r.returncode, " ".join(cmd)))
    return r


def newest(paths):
    return max(os.path.getmtime(p) for p in paths)


def remove_tree(path):
    def retry(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onerror=retry)


def is_contra(data):
    return len(data) == ROM_SIZE and hashlib.sha512(data).hexdigest() == ROM_SHA512


def describe(data):
    return "%d bytes, MD5 %s, CRC32 %08X" % (len(data), hashlib.md5(data).hexdigest().upper(),
                                             zlib.crc32(data) & 0xFFFFFFFF)


# ---------------------------------------------------------- the upstreams

def git_rev(path):
    if not os.path.exists(os.path.join(path, ".git")):
        return None
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    return r.stdout.strip() if r.returncode == 0 else None


def fetch(url, rev, path, what):
    """`url` at `rev` in `path`: only that commit, without history."""
    have = git_rev(path)
    if have == rev:
        return
    if shutil.which("git") is None:
        die("git not found (install Git for Windows: https://git-scm.com/download/win)")
    if have is None:
        if os.path.isdir(path):
            remove_tree(path)
        os.makedirs(path)
        say("fetching %s (%s)" % (what, rev[:12]))
        run(["git", "init", "-q", path])
        run(["git", "-C", path, "remote", "add", "origin", url])
    else:
        say("moving %s from %s to %s" % (what, have[:12], rev[:12]))
    run(["git", "-C", path, "fetch", "-q", "--depth", "1", "origin", rev])
    run(["git", "-C", path, "checkout", "-q", "--detach", "FETCH_HEAD"])
    if git_rev(path) != rev:
        die("%s is not at %s after the fetch" % (path, rev))


# ----------------------------------------------------------- the cartridge

def find_user_rom():
    for n in ROM_NAMES:
        p = os.path.join(ROOT, n)
        if os.path.isfile(p):
            return p
    return None


def read_user_rom():
    """Your cartridge, as the disassembly builds it: (path, bytes). A dump
    whose header differs only in the bytes iNES leaves unused is taken with
    the disassembly's header."""
    user = find_user_rom()
    if not user:
        die("no baserom.nes: put your own Contra (US) cartridge in this folder as baserom.nes "
            "(MD5 %s). The graphics and music are taken from it." % ROM_MD5.upper())
    with open(user, "rb") as f:
        data = f.read()
    name = os.path.basename(user)
    if is_contra(data):
        return user, data
    if len(data) == ROM_SIZE and data[:4] == INES_HEADER[:4]:
        fixed = INES_HEADER + data[16:]
        if is_contra(fixed):
            say("%s: Contra (US), its header's unused bytes cleaned" % name)
            return user, fixed
    if hashlib.md5(data).hexdigest() == PROBOTECTOR_MD5:
        die("%s is Probotector, the PAL version: this port is of Contra (US), and runs the NES at "
            "NTSC's speed. Use a Contra (US) cartridge." % name)
    die("%s is not Contra (US) (%s; the cartridge is %d bytes, MD5 %s)." %
        (name, describe(data), ROM_SIZE, ROM_MD5.upper()))


def step_cc65():
    """ca65 and ld65, from cc65's own sources (no MSYS, no make)."""
    common = sorted(os.path.join(CC65, "src", "common", f)
                    for f in os.listdir(os.path.join(CC65, "src", "common")) if f.endswith(".c"))
    for tool, exe in (("ca65", CA65_EXE), ("ld65", LD65_EXE)):
        d = os.path.join(CC65, "src", tool)
        srcs = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c")) + common
        if os.path.isfile(exe) and os.path.getmtime(exe) >= newest(srcs):
            continue
        say("building cc65's %s" % tool)
        os.makedirs(BUILD, exist_ok=True)
        ok, how = xbconfig.build_host_program(srcs, exe, BUILD,
                                              extra=["-I" + os.path.join(CC65, "src", "common")])
        if not ok:
            die("could not build %s: %s" % (tool, how))
        say("%s built with %s" % (tool, how))


def extract_assets(work, rom):
    """The graphics and the music, from the cartridge at the offsets
    assets.txt gives (file, offset, length), into the copy's src/assets."""
    n = 0
    with open(os.path.join(work, "assets.txt"), encoding="utf-8") as f:
        for line in f:
            t = line.split()
            if len(t) < 3:
                continue
            path = os.path.join(work, *t[0].replace("/", "\\").split("\\"))
            off, length = int(t[1]), int(t[2])
            if off + length > len(rom):
                die("assets.txt: %s lies past the cartridge's end" % t[0])
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as out:
                out.write(rom[off:off + length])
            n += 1
    return n


def assemble(rom):
    """A copy of the disassembly in build/decomp (the checkout untouched),
    its assets from `rom`; ca65 on each file, ld65 with contra.cfg, as the
    disassembly's build.ps1 does. The cartridge's bytes."""
    work = os.path.join(BUILD, "decomp")
    if os.path.isdir(work):
        remove_tree(work)
    os.makedirs(work)
    shutil.copytree(os.path.join(DECOMP, "src"), os.path.join(work, "src"))
    for f in ("contra.cfg", "assets.txt"):
        shutil.copy(os.path.join(DECOMP, f), work)
    n = extract_assets(work, rom)
    os.makedirs(os.path.join(work, "obj"))
    for o in OBJECTS:
        r = subprocess.run([CA65_EXE, "-D", "Contra", "--debug-info", "-o", os.path.join("obj", o + ".o"),
                            os.path.join("src", o + ".asm")], cwd=work, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if r.returncode != 0:
            sys.stderr.write(r.stdout + r.stderr)
            die("ca65 could not assemble src/%s.asm" % o)
    out = os.path.join(work, "contra.nes")
    r = subprocess.run([LD65_EXE, "-C", "contra.cfg", "--dbgfile", "contra.dbg"] +
                       [os.path.join("obj", o + ".o") for o in OBJECTS] + ["-o", "contra.nes"],
                       cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0 or not os.path.isfile(out):
        sys.stderr.write(r.stdout + r.stderr)
        die("ld65 could not link the cartridge")
    with open(out, "rb") as f:
        data = f.read()
    return data, n


def put_rom(data):
    """build/Contra.nes; left alone (and its time with it) when unchanged"""
    if os.path.isfile(ROM_OUT):
        with open(ROM_OUT, "rb") as f:
            if f.read() == data:
                return
    with open(ROM_OUT, "wb") as f:
        f.write(data)


def step_rom(assemble_it):
    """build/Contra.nes."""
    os.makedirs(BUILD, exist_ok=True)
    user, rom = read_user_rom()
    say("%s: Contra (US) (%s)" % (os.path.basename(user), describe(rom)))
    if not assemble_it:
        put_rom(rom)
        say("build/Contra.nes: your cartridge, as it is (--no-assemble)")
        return
    fetch(DECOMP_URL, DECOMP_REV, DECOMP, "the Contra (US) disassembly")
    fetch(CC65_URL, CC65_REV, CC65, "cc65 2.19")
    step_cc65()
    say("assembling the cartridge from the disassembly")
    data, n = assemble(rom)
    if not is_contra(data):
        die("the assembled cartridge is not Contra (US) (%s)" % describe(data))
    say("the disassembly is the cartridge byte for byte (%d assets from your cartridge; %s)" %
        (n, describe(data)))
    put_rom(data)


# ------------------------------------------------------- hosttest, icons

def step_hosttest():
    core = os.path.join(ROOT, "src", "core")
    srcs = [os.path.join(ROOT, "tools", "hosttest", f) for f in ("hosttest.c", "netplaytest.c")] + \
        sorted(os.path.join(core, f) for f in os.listdir(core) if f.endswith(".c"))
    hdrs = [os.path.join(core, f) for f in os.listdir(core) if f.endswith(".h")]
    if os.path.isfile(HOSTTEST_EXE) and os.path.getmtime(HOSTTEST_EXE) >= newest(srcs + hdrs):
        return
    say("building hosttest, the core on the PC")
    ok, how = xbconfig.build_host_program(srcs, HOSTTEST_EXE, BUILD)
    if not ok:
        die("could not build hosttest: %s" % how)


def step_icons():
    """Original dashboard artwork; no game screenshot or cartridge assets."""
    run([sys.executable, os.path.join(ROOT, "tools", "xbox", "make_icons.py")])


# ------------------------------------------------------------------ data

def step_data():
    """data/: only the cartridge (anything else there would go on the disc)."""
    os.makedirs(DATA, exist_ok=True)
    dst = os.path.join(DATA, "Contra.nes")
    with open(ROM_OUT, "rb") as f:
        rom = f.read()
    have = None
    if os.path.isfile(dst):
        with open(dst, "rb") as f:
            have = f.read()
    if have != rom:
        shutil.copyfile(ROM_OUT, dst)
    for dp, dn, fn in os.walk(DATA, topdown=False):
        for f in fn:
            p = os.path.join(dp, f)
            if os.path.normcase(p) != os.path.normcase(dst):
                os.remove(p)
        if dp != DATA and not os.listdir(dp):
            os.rmdir(dp)


def tool(name, *args):
    run([sys.executable, os.path.join(ROOT, "tools", "xbox", name)] + list(args))


def step_xbox():
    tool("gen_manifest.py")
    if os.path.isfile(xbconfig.rxdk_cli()):
        tool("gen_vcxproj.py")


def step_build(config):
    cli = xbconfig.rxdk_cli()
    if not os.path.isfile(cli):
        die("Rxdk.Cli.exe not found: install RXDK (RXDK > RXDK Window > Install Prerequisites)")
    # the build copies data/ into its output and never removes anything
    # there: what data/ no longer has would still go onto the disc
    out_data = os.path.join(ROOT, "out", config, "Build", NAME, "data")
    for dp, dn, fn in os.walk(out_data, topdown=False):
        for f in fn:
            rel = os.path.relpath(os.path.join(dp, f), out_data)
            if not os.path.isfile(os.path.join(DATA, rel)):
                os.remove(os.path.join(dp, f))
    say("building %s" % config)
    run([cli, "build", "--project-root", ROOT, "--configuration", config])
    iso = os.path.join(ROOT, "out", config, "XISO", NAME + ".iso")
    if os.path.isfile(iso):
        say("done: %s (%.1f MB)" % (iso, os.path.getsize(iso) / 1048576.0))
    else:
        say("built; the ISO is under out/%s/XISO" % config)


# ----------------------------------------------------------------- check

def check():
    ok = True
    user = find_user_rom()
    if user:
        with open(user, "rb") as f:
            data = f.read()
        good = is_contra(data) or (len(data) == ROM_SIZE and is_contra(INES_HEADER + data[16:]))
        say("cartridge: %s, %s" % (os.path.basename(user), "Contra (US)" if good
                                   else "NOT Contra (US) (%s)" % describe(data)))
        ok &= good
    else:
        say("cartridge: missing -- put your Contra (US) cartridge here as baserom.nes")
        ok = False
    say("disassembly: %s" % ("fetched" if git_rev(DECOMP) == DECOMP_REV else "fetched by the build"))
    py = sys.version_info >= (3, 8)
    say("Python: %d.%d%s" % (sys.version_info[0], sys.version_info[1], "" if py else " (3.8 or later needed)"))
    ok &= py
    g = shutil.which("git")
    say("git: %s" % (g or "missing (install Git for Windows; or build with --no-assemble)"))
    cli = xbconfig.rxdk_cli()
    there = os.path.isfile(cli)
    say("RXDK CLI: %s" % (cli if there else "missing (RXDK > RXDK Window > Install Prerequisites)"))
    ok &= there
    hc = xbconfig.host_cc_available()
    say("host C compiler: %s" % (hc or "missing (the RXDK's clang, Visual Studio or gcc)"))
    ok &= hc is not None
    for label, path in (("build/Contra.nes", ROM_OUT), ("hosttest", HOSTTEST_EXE),
                        ("icons", os.path.join(ROOT, "Platform", "xbox", "gen", "titleimage.bmp")),
                        ("data", os.path.join(DATA, "Contra.nes")),
                        ("manifest", os.path.join(ROOT, "rxdk.project.json"))):
        say("%s: %s" % (label, "built" if os.path.exists(path) else "not built yet"))
    return ok


def main():
    ap = argparse.ArgumentParser(description="the Contra (US) disassembly -> an Xbox disc image")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--no-assemble", action="store_true",
                    help="use baserom.nes as it is (checked), without fetching or assembling")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--config", default="Release", choices=["Release", "Debug"])
    a = ap.parse_args()
    if sys.version_info < (3, 8):
        die("Python 3.8 or later is needed")
    if a.check:
        sys.exit(0 if check() else 1)

    step_rom(not a.no_assemble)
    step_hosttest()
    run([HOSTTEST_EXE, ROM_OUT, "--netplay-test"])
    step_icons()
    step_data()
    step_xbox()
    if not a.skip_build:
        step_build(a.config)


if __name__ == "__main__":
    main()
