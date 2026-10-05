"""Build the production DLL offline using an explicitly supplied LLVM-MinGW bin."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from package import DLL_NAME, ROOT, dependencies, output_directory, sha, source_files, version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True, help="LLVM-MinGW bin directory")
    parser.add_argument("--out", type=Path, default=ROOT / "out/release")
    args = parser.parse_args()
    out = output_directory(ROOT, args.out, "out")
    out.mkdir(parents=True, exist_ok=True)
    state_path = out / "build.json"
    state_path.write_text(json.dumps(dict(status="BUILDING")), encoding="utf-8")
    toolchain = args.toolchain.resolve()
    cc = toolchain / "x86_64-w64-mingw32-clang.exe"
    cxx = toolchain / "x86_64-w64-mingw32-clang++.exe"
    mh = ROOT / "third_party/minhook"
    rf = ROOT / "third_party/reframework/include"
    deps = dependencies(ROOT)
    sources = source_files(ROOT)
    release_version = version(ROOT)

    def run(command, name):
        result = subprocess.run([str(a) for a in command], cwd=out,
                                capture_output=True, timeout=180)
        (out / (name + ".log")).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + " failed:\n" +
                               (result.stdout + result.stderr).decode("utf-8", errors="replace"))
        print(name + " PASS", flush=True)
        return result.stdout.decode("utf-8", errors="replace")

    compiler = run([cxx, "--version"], "compiler_version").splitlines()[0]
    with tempfile.TemporaryDirectory(prefix="source_", dir=out) as temporary:
        src = Path(temporary)
        for relative, data in sources.items():
            target = src / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        objects = []
        for relative in ("src/buffer.c", "src/hde/hde32.c", "src/hde/hde64.c",
                         "src/hook.c", "src/trampoline.c"):
            obj = out / relative.replace("/", "_").replace(".c", ".o")
            run([cc, "-std=c11", "-O2", "-DNDEBUG", "-Werror",
                 "-DMINHOOK_ENABLE_FIXED16_FALLBACK", "-DMINHOOK_MAX_MEMORY_RANGE=0x7FFF0000ULL",
                 "-DMINHOOK_ENABLE_ALLOC_DIAGNOSTICS", "-I", mh / "include",
                 "-I", mh / "src/hde", "-c", mh / relative, "-o", obj], "compile_" + obj.name)
            objects.append(obj)
        run([cxx, "-std=c++23", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-static",
             "-DLG_LOOSE_CLEANUP", "-I", src, "-I", mh / "include", "-I", rf,
             "-Wl,--no-insert-timestamp", "-fms-extensions", "-shared",
             src / "DStorageFileLifetimeGuard.cpp", *objects, "-o", out / DLL_NAME], "compile_dll")
    if sources != source_files(ROOT) or deps != dependencies(ROOT):
        raise ValueError("Build inputs changed while compilation was running")
    state = dict(status="BUILT", version=release_version, compiler=compiler,
                 dll_sha256=sha((out / DLL_NAME).read_bytes()),
                 source={name: sha(data) for name, data in sources.items()}, dependencies=deps)
    state_path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: state[key] for key in ("status", "version", "dll_sha256")}, indent=2))


if __name__ == "__main__":
    main()
