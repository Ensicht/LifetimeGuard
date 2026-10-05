# Third-Party Dependencies

## MinHook

`minhook/` contains a modified MinHook implementation. Upstream project:
[TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook).

Local extensions provide allocation diagnostics, near-allocation range and race
handling, and a narrowly gated fixed-16-byte fallback with unwind and thread-IP
handling. These extensions are part of the build input, not interchangeable with
an arbitrary upstream binary. Content hashes are pinned in `manifest.json`.

Original authors and BSD-style notices, including Hacker Disassembler Engine
notices, remain in `minhook/AUTHORS.txt`, `minhook/LICENSE.txt`, the source headers,
and `../licenses/MinHook.txt`. Separable Laz modifications use the project MIT
license; upstream notices still apply to derived files.

## REFramework

Only the C plugin API header is included. Upstream project:
[praydog/REFramework](https://github.com/praydog/REFramework).
The header uses the MIT license, Copyright 2019 praydog; see
`../licenses/REFramework.txt`. The plugin consumes the host C ABI and exported UI
functions; this package does not bundle the REFramework runtime or ImGui.

## Compiler Runtime

The external toolchain is `llvm-mingw-20260616-ucrt-x86_64`, Clang 22.1.8.
The toolchain is not included. Keep its complete distribution and original
licenses. The DLL statically links runtime components; redistribution notices
are included in `../licenses/LLVM.txt`, `../licenses/COPYING.winpthreads.txt`,
and `../licenses/COPYING.MinGW-w64-runtime.txt`.

Production compilation requires no DirectStorage SDK or game files. Runtime
compatibility requirements are described in the root README. Build commands and
output descriptions are in `../docs/BUILD_zh.md`.
