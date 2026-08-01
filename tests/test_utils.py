"""Shared utilities for test scripts: PE arch detection, ASan DLL PATH, binary discovery."""
import glob, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")

def pe_arch(path):
    """Detect PE machine type: 'x86' or 'x64', or None."""
    try:
        with open(path, 'rb') as f:
            if f.read(2) != b'MZ':
                return None
            f.seek(0x3C)
            pe_off = int.from_bytes(f.read(4), 'little')
            f.seek(pe_off)
            if f.read(4) != b'PE\0\0':
                return None
            machine = int.from_bytes(f.read(2), 'little')
            if machine == 0x14C:
                return 'x86'
            if machine == 0x8664:
                return 'x64'
            return None
    except Exception:
        return None

def build_env(bin_path):
    """Add MSVC ASan DLL directory to PATH based on binary architecture."""
    env = os.environ.copy()
    # Use ASAN_DLL_DIR from CI if set
    asan_dir = os.environ.get("ASAN_DLL_DIR")
    if asan_dir and os.path.isdir(asan_dir):
        env["PATH"] = asan_dir + os.pathsep + env["PATH"]
        return env

    msvc_root = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
    versions = sorted(glob.glob(os.path.join(msvc_root, "*")), reverse=True)

    arch = pe_arch(bin_path) if os.path.isfile(bin_path) else None
    subdirs = ['x64', 'x86'] if arch is None else [{'x86': 'x86', 'x64': 'x64'}[arch]]

    for v in versions:
        for sd in subdirs:
            dll_dir = os.path.join(v, "bin", "Hostx64", sd)
            asan_name = "clang_rt.asan_dynamic-x86_64.dll" if sd == 'x64' else "clang_rt.asan_dynamic-i386.dll"
            if os.path.isfile(os.path.join(dll_dir, asan_name)):
                env["PATH"] = dll_dir + os.pathsep + env["PATH"]
                return env
    return env

def find_tool(name):
    """Locate a test binary (echo_test, long_session, etc.) across build dirs."""
    for bd in ("build", "build-release", "build-x64", "build-x86",
               "build-asan", "build-asan-x64", "build-asan-x86"):
        for sub in ("Release", "."):
            path = os.path.join(ROOT, bd, sub, name + (".exe" if os.name == "nt" else ""))
            if os.path.isfile(path):
                return os.path.abspath(path)
    # fallback
    fallback = name + (".exe" if os.name == "nt" else "")
    return os.path.join(ROOT, "build", "Release", fallback) if os.name == "nt" else os.path.join(ROOT, "build", name)

def find_echo_test():
    return find_tool("echo_test")
