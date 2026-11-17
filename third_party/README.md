# Third-Party Dependencies

## MinHook

`minhook/` is the exact local MinHook implementation used by the validated LifetimeGuard v0.5.4 baseline. Upstream: <https://github.com/TsudaKageyu/minhook>.

This is **not an unmodified upstream release**. It includes the already validated allocation diagnostics, corrected near-allocation range/race handling, and narrowly validated fixed-16-byte fallback with unwind/thread-IP handling. Do not replace it with stock MinHook without rerunning the native fault-injection suite. Source-file hashes are pinned in `manifest.json`.

Original authors and BSD-style license (including Hacker Disassembler Engine notices) remain in `minhook/AUTHORS.txt`, `minhook/LICENSE.txt` and `../licenses/MinHook.txt`. Laz modifications follow the project MIT license where separable; upstream notices still apply to derived files.

## REFramework

Only the C plugin API header is vendored, identical to the stable build input. Upstream: <https://github.com/praydog/REFramework>. License: MIT, Copyright 2019 praydog; see `../licenses/REFramework.txt`.

The game plugin uses that C ABI and the host's exported UI functions; it does not bundle REFramework or link a separate ImGui implementation.

## External Test Inputs

The DirectStorage 1.2.3 SDK headers/import library, the game's `dstorage.dll` / `dstoragecore.dll`, and `MonsterHunterWilds.exe` are **not distributed** in this repository or install packages. Supply your own valid installation and SDK when running the native suite. A production DLL build does not require the DirectStorage SDK or game binaries.

The toolchain is external llvm-mingw; release verification uses `llvm-mingw-20260616-ucrt-x86_64`. Keep its original distribution and licenses separately. The release DLL statically links its runtime; runtime redistribution notices must accompany binary releases.
