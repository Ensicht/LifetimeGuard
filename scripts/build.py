"""Build the shipping DLL and isolated native harnesses. Never deploy to a game."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

from package import DLL_NAME, WMP_DLL_NAME, source_files

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Build:
    def __init__(self, args):
        self.args = args
        self.out = args.out.resolve()
        if not self.out.is_relative_to(ROOT / "out"):
            raise ValueError("Build output must be beneath this repository's out directory")
        self.out.mkdir(parents=True, exist_ok=True)
        # A failed rebuild must not leave an earlier successful release certificate active.
        (self.out / "build.json").write_text(json.dumps(dict(status="BUILDING", tests_passed=False)), encoding="utf-8")
        if args.test:
            (self.out / "tests.json").write_text(json.dumps(dict(passed=False, status="PENDING_BUILD")), encoding="utf-8")
        self.bin = args.toolchain.resolve()
        self.cc = self.bin / "x86_64-w64-mingw32-clang.exe"
        self.cxx = self.bin / "x86_64-w64-mingw32-clang++.exe"
        self.mh = ROOT / "third_party/minhook"
        self.rf = ROOT / "third_party/reframework/include"
        self.src = self.out / "source"
        self.src.mkdir(exist_ok=True)
        source_root = args.source.resolve() if args.source else ROOT / "src"
        self.sources = source_files(source_root)
        if source_files(self.src).keys() - self.sources.keys():
            raise ValueError("Build source directory has stale files; use a fresh --out")
        for name, original in self.sources.items():
            target = self.src / name
            target.parent.mkdir(parents=True, exist_ok=True)
            text = original.read_text("utf-8-sig")
            if args.label and name == "GuardBuild.h":
                text = re.sub(r'(#define LG_BUILD_TEXT )"[^"]+"', r'\g<1>"' + args.label + '"', text)
            target.write_text(text, encoding="utf-8", newline="\n")
        self.version = re.search(r'#define LG_BUILD_TEXT "([^"]+)"', (self.src / "GuardBuild.h").read_text("utf-8")).group(1)
        self.calls = []
        self.common = ["-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-static",
                       "-DLG_LOOSE_CLEANUP", "-I", str(self.src), "-I", str(self.mh / "include"),
                       "-I", str(self.rf), "-Wl,--no-insert-timestamp"]

    def run(self, args, name, timeout=180):
        args = [str(a) for a in args]
        result = subprocess.run(args, cwd=self.out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout)
        (self.out / (name + ".log")).write_bytes(result.stdout)
        self.calls.append(dict(name=name, returncode=result.returncode))
        if result.returncode:
            print(result.stdout.decode("utf-8", errors="replace"))
            raise RuntimeError(f"{name} failed ({result.returncode})")
        print(name + " PASS", flush=True)
        return result.stdout

    def c_object(self, source, output, flags=()):
        self.run([self.cc, "-std=c11", "-O2", "-DNDEBUG", "-Werror", *flags,
                  "-I", self.mh / "include", "-I", self.mh / "src/hde",
                  "-c", source, "-o", self.out / output], "compile_" + output)
        return self.out / output

    def cpp(self, name, extra=(), objects=(), flags=()):
        source = ROOT / "tests" / (name + ".cpp")
        self.run([self.cxx, "-std=c++23", *self.common, *flags, source, *extra, *objects,
                  "-o", self.out / (name + ".exe")], "compile_" + name)

    def execute(self):
        manifest = json.loads((ROOT / "third_party/manifest.json").read_text("utf-8"))
        for relative, expected in manifest.items():
            if digest(ROOT / relative) != expected:
                raise ValueError("Dependency changed: " + relative)
        mh_flags = ["-DMINHOOK_ENABLE_FIXED16_FALLBACK", "-DMINHOOK_MAX_MEMORY_RANGE=0x7FFF0000ULL",
                    "-DMINHOOK_ENABLE_ALLOC_DIAGNOSTICS"]
        objects = []
        for relative in ["src/buffer.c", "src/hde/hde32.c", "src/hde/hde64.c", "src/hook.c", "src/trampoline.c"]:
            objects.append(self.c_object(self.mh / relative, relative.replace("/", "_").replace(".c", ".o"), mh_flags))
        native = [self.cxx, "-std=c++23", *self.common, "-fms-extensions", "-shared"]
        companion = []
        if "BoneSystem/Bridge.cpp" in self.sources:
            companion = ["-DLG_BONE_SYSTEM_COMPANION", self.src / "BoneSystem/Bridge.cpp", "-lbcrypt"]
        self.run([*native, self.src / "DStorageFileLifetimeGuard.cpp", *objects, *companion,
                  "-o", self.out / DLL_NAME], "compile_dll")
        binaries = [DLL_NAME]
        if "WmpProvider.cpp" in self.sources:
            self.run([*native, self.src / "WmpProvider.cpp", *objects, *companion,
                      "-o", self.out / WMP_DLL_NAME], "compile_wmp_dll")
            binaries.append(WMP_DLL_NAME)
        if self.args.test:
            if not self.args.game_dir or not self.args.dstorage_sdk:
                raise ValueError("Native tests require --game-dir and --dstorage-sdk")
            game, sdk = self.args.game_dir.resolve(), self.args.dstorage_sdk.resolve()
            for name in ["dstorage.dll", "dstoragecore.dll"]:
                shutil.copy2(game / name, self.out / name)
            sdk_flags = ["-I", str(sdk / "include"), "-fms-extensions", "-municode"]
            library = sdk / "lib/x64/dstorage.lib"
            for name in ["DStorageFileLifetimeGuardHarness", "DStorageFileLifetimeGuardStress", "DStorageLooseCleanupTest"]:
                self.cpp(name, extra=[library], flags=sdk_flags)
            for name in ["DStorageFailurePolicyTest", "DStorageDetachedTextureDllHarness",
                         "DStorageDetachedTextureBoundaryTest", "DStorageGameplayTextureRetryTest",
                         "DStoragePointerHookTransactionTest", "DStorageStatusUiTest",
                         "DStorageLoadingGateSimulation", "DStoragePostPlayerEndProtectionSimulation",
                         "DStorageResourceRecoverySimulation", "DStorageResourceRecoveryNegativeHost",
                         "DStorageDetachedTextureProtocolTest"]:
                self.cpp(name)
            self.cpp("DStorageDetachedNativeInstallHarness", flags=["-Wl,--image-base,0x180000000"])
            self.cpp("DStorageLifecycleIntegrationHarness", flags=["-fms-extensions"])
            self.cpp("DStorageResourceSignatureVerifier", flags=["-fms-extensions", "-municode"])
            self.cpp("DStorageAllocationDiagnosticThreadTest", objects=[objects[0]], flags=["-I", str(self.mh / "src")])
            assembly = self.out / "DStorageResourceRecoverySynthetic.o"
            self.run([self.cc, "-c", ROOT / "tests/DStorageResourceRecoverySynthetic.S", "-o", assembly], "compile_assembly")
            self.cpp("DStorageResourceRecoveryHookHarness", objects=[*objects, assembly])
            legacy = self.c_object(self.mh / "src/buffer.c", "src_buffer_legacy.o", ["-DMINHOOK_MAX_MEMORY_RANGE=0x40000000ULL"])
            for label, buffer in [("Legacy", legacy), ("Fixed", objects[0])]:
                self.run([self.cxx, "-std=c++23", *self.common, ROOT / "tests/DStorageMinHookNearAllocationHarness.cpp",
                          buffer, *objects[1:], "-o", self.out / ("DStorageMinHookNearAllocation" + label + ".exe")], "compile_near_" + label)
            bridge = self.c_object(ROOT / "tests/DStorageFixed16TestBridge.c", "Fixed16TestBridge.o")
            self.cpp("DStorageFixed16Harness", objects=[bridge, *[o for o in objects if o.name != "src_hook.o"]],
                     flags=["-Wl,--image-base,0x180000000"])
            no_near = self.c_object(ROOT / "tests/DStorageNoNearBuffer.c", "NoNearTestBuffer.o", ["-DMINHOOK_ENABLE_ALLOC_DIAGNOSTICS"])
            self.run([self.cxx, "-std=c++23", *self.common, "-fms-extensions", "-shared",
                      self.src / "DStorageFileLifetimeGuard.cpp", no_near, *objects[1:],
                      "-o", self.out / "DStorageFileLifetimeGuardNoNearTest.dll"], "compile_fault_injection_dll")
            self.run([sys.executable, "-B", ROOT / "scripts/test.py", "--build", self.out,
                      "--game", game, "--version", self.version], "acceptance", timeout=1800)
        result = dict(status="BUILT", version=self.version, tests_passed=self.args.test,
                      binaries={name: digest(self.out / name) for name in binaries},
                      dependencies=manifest, calls=self.calls,
                      source={name: digest(self.src / name) for name in self.sources})
        (self.out / "build.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps({k: result[k] for k in ["version", "binaries", "tests_passed"]}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True, help="llvm-mingw bin directory")
    parser.add_argument("--out", type=Path, default=ROOT / "out/release")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--dstorage-sdk", type=Path, help="DirectStorage 1.2.3 native directory")
    parser.add_argument("--source", type=Path, help="Offline baseline source override")
    parser.add_argument("--label", help="Offline version-label normalization")
    Build(parser.parse_args()).execute()


if __name__ == "__main__":
    main()
