"""Pinned upstream loader integration probe, not a Magpie/NGX/GPU acceptance test.

Windows x64, Python 3. Each DLL load runs in a fresh bounded subprocess;
private copies/config/package trees/logs are retained in --output for inspection. No
product directories, default caches or running Magpie processes are changed.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def contract():
    header = (Path(__file__).resolve().parents[1] /
              "src/Shared/DlssgSm86Package.h").read_text(encoding="utf-8")
    table = re.search(r"File Files\[\]\{(.*?)\n\};", header, re.S).group(1)
    records = {name: (int(size), digest) for name, size, digest in
               re.findall(r'\{ L"([\w./-]+)", (\d+), "([a-f0-9]{64})" \}', table)}
    return records


def verify(path, expected):
    data = path.read_bytes()
    if (len(data), hashlib.sha256(data).hexdigest()) != expected:
        raise RuntimeError(f"Identity mismatch: {path}")


def child(directory, request):
    k = c.WinDLL("kernel32", use_last_error=True)
    k.LoadLibraryExW.argtypes = [w.LPCWSTR, w.HANDLE, w.DWORD]
    k.LoadLibraryExW.restype = w.HMODULE
    k.GetModuleFileNameW.argtypes = [w.HMODULE, w.LPWSTR, w.DWORD]
    k.GetModuleFileNameW.restype = w.DWORD
    k.GetProcAddress.argtypes = [w.HMODULE, c.c_char_p]
    k.GetProcAddress.restype = c.c_void_p
    k.GetFileAttributesExW.argtypes = [w.LPCWSTR, c.c_int, c.c_void_p]
    k.GetFileAttributesExW.restype = w.BOOL
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    k.GetCurrentProcess.restype = w.HANDLE
    k.K32EnumProcessModules.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD, c.POINTER(w.DWORD)]
    k.K32EnumProcessModules.restype = w.BOOL
    k.GetModuleHandleExW.argtypes = [w.DWORD, c.c_void_p, c.POINTER(w.HMODULE)]
    k.GetModuleHandleExW.restype = w.BOOL

    def mapped(module):
        buffer = c.create_unicode_buffer(32768)
        length = k.GetModuleFileNameW(module, buffer, len(buffer))
        if not length or length >= len(buffer):
            raise c.WinError(c.get_last_error())
        return Path(buffer.value)

    # Exercise the production sharing mode, including files held open at load.
    def lock(path):
        handle = k.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x80, None)
        if handle == c.c_void_p(-1).value:
            raise c.WinError(c.get_last_error())
        return handle

    records = contract()
    proxy_path = directory / "version.dll"
    verify(proxy_path, records["version.dll"])
    held_files = [lock(directory / name) for name in records]
    held_files.append(lock(directory / "dlssg_sm86.ini"))
    for name, expected in records.items():
        verify(directory / name, expected)
    proxy = k.LoadLibraryExW(str(proxy_path), None, 0x100 | 0x800)
    if not proxy or not k.GetProcAddress(proxy, b"DlssgProxy_Name"):
        raise RuntimeError(f"Proxy load failed: {c.get_last_error()}")
    if not mapped(proxy).samefile(proxy_path):
        raise RuntimeError("Wrong proxy mapping")
    result = {"proxy": str(mapped(proxy)), "requested": request, "locked_files": len(held_files)}
    if request:
        target = directory / "features/nvngx_dlssg.dll"
        attributes = c.create_string_buffer(128)
        existed = bool(k.GetFileAttributesExW(str(target), 0, attributes))
        runtime = k.LoadLibraryExW(str(target), None, 0x100 | 0x800)
        if not runtime:
            raise RuntimeError(f"Runtime request failed: {c.get_last_error()}")
        actual = mapped(runtime)
        if actual.parent == target.parent or actual.name.lower() != "nvngx_dlssg.dll":
            raise RuntimeError("Request did not redirect to the bundle")
        if not actual.is_relative_to(directory / "dlssg_sm86"):
            raise RuntimeError(f"Bundle escaped the private test cache (INI encoding/path): {actual}")
        present_after = bool(k.GetFileAttributesExW(str(target), 0, attributes))
        missing_error = c.get_last_error()
        if present_after != existed or (not existed and missing_error != 2):
            raise RuntimeError("Request path presence changed after redirection")
        verify(actual, records["dlssg_sm86/runtime/nvngx_dlssg.dll"])
        modules = (w.HMODULE * 1024)()
        needed = w.DWORD()
        if not k.K32EnumProcessModules(k.GetCurrentProcess(), modules, c.sizeof(modules), c.byref(needed)):
            raise c.WinError(c.get_last_error())
        if needed.value > c.sizeof(modules):
            raise RuntimeError("Module buffer too small")
        pair = {}
        for module in modules[:needed.value // c.sizeof(w.HMODULE)]:
            path = mapped(module)
            if not path.name.lower().startswith(("nvngx_dlssg", "sm86_backend")):
                continue
            expected_name = ("dlssg_sm86/runtime/nvngx_dlssg.dll" if path.name == "nvngx_dlssg.dll"
                             else "dlssg_sm86/backend/sm86_backend.dll")
            if path != directory / expected_name or path.name in pair:
                raise RuntimeError(f"Unexpected pair mapping: {path}")
            verify(path, records[expected_name])
            held_files.append(lock(path))
            pinned = w.HMODULE()
            if not k.GetModuleHandleExW(1 | 4, module, c.byref(pinned)) or pinned.value != module:
                raise RuntimeError("Failed to pin mapped module")
            pair[path.name] = str(path)
        if set(pair) != {"nvngx_dlssg.dll", "sm86_backend.dll"}:
            raise RuntimeError("Incomplete bundle pair")
        result.update(runtime=str(actual), pair=pair, locked_files=len(held_files),
                      request_path_existed=existed, request_path_present_after=present_after)
    print(json.dumps(result, ensure_ascii=True), flush=True)
    # Never FreeLibrary a process-wide proxy/backend. The child exits normally.


def events(directory):
    result = []
    for path in (directory / "dlssg_sm86/logs").glob("*.jsonl"):
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                result.append(json.loads(line))
            except json.JSONDecodeError:
                raise RuntimeError(f"Malformed upstream log: {path}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New directory; must not exist. Evidence is not deleted.")
    parser.add_argument("--route", choices=("native", "forward"), required=True,
                        help="Match actual test GPU: native SM75/86, newer PTX SM89/120")
    args = parser.parse_args()
    if sys.platform != "win32" or c.sizeof(c.c_void_p) != 8:
        raise RuntimeError("Requires Windows x64 Python")
    records = contract()
    for name, record in records.items():
        verify(args.runtime_directory / name, record)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    summary = []
    for name, present, request in (("proxy-only", False, False),
                                    ("request-missing", False, True),
                                    ("request-present", True, True)):
        directory = args.output / ("中文 space " + name)
        (directory / "features").mkdir(parents=True)
        for relative in records:
            target = directory / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.runtime_directory / relative, target)
        ini = (args.runtime_directory / (args.route + ".ini")).read_bytes()
        (directory / "dlssg_sm86.ini").write_bytes(ini)
        if present:
            shutil.copyfile(args.runtime_directory / "dlssg_sm86/runtime/nvngx_dlssg.dll",
                            directory / "features/nvngx_dlssg.dll")
        # A second fresh process observes the same immutable warm bundle.
        for temperature in (("cold", "warm") if name == "request-missing" else ("cold",)):
            process = subprocess.run([sys.executable, str(Path(__file__).resolve()), "child",
                                      str(directory), str(int(request))], cwd=directory,
                                     capture_output=True, text=True, encoding="utf-8", timeout=30)
            item = {"case": name, "temperature": temperature, "exit_code": process.returncode,
                    "stdout": process.stdout, "stderr": process.stderr}
            (directory / (temperature + "-result.json")).write_text(json.dumps(item, indent=2), encoding="utf-8")
            summary.append(item)
            (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
            if process.returncode:
                raise RuntimeError(f"{name}/{temperature}: child failed; see {directory}")
            if not request and events(directory):
                raise RuntimeError("Proxy-only case unexpectedly activated the bundle")
            # Pair identity is proven above. Upstream active/kernel/NGX acceptance
            # must still be inspected separately; do not infer it from mapping.
    print(json.dumps({"passed": len(summary), "output": str(args.output),
                      "scope": "upstream loader, redirected pair identities, file locks and repeated immutable package loads only; no NGX calls"}))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "child":
        child(Path(sys.argv[2]), sys.argv[3] == "1")
    else:
        main()
