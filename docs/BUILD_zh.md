# LifetimeGuard 构建说明

## 环境与依赖

- Windows x64。
- 完整 Python 3.11 或更高版本，包含标准库的 zlib 模块；不需要第三方包。部分工具链附带的精简 Python 缺少 zlib，不能用于 ZIP 打包。
- 完整解压的 `llvm-mingw-20260616-ucrt-x86_64` 工具链，Clang 22.1.8，UCRT、POSIX 线程运行库。保留工具链完整目录结构，不要只复制 `bin` 中的编译器。
- 源码包已包含所需 MinHook 源码与 REFramework C API 头文件；固定内容哈希见 `third_party/manifest.json`，许可见 `LICENSE`、`licenses/` 和第三方原文件。

构建不需要游戏安装、DirectStorage SDK、已安装的 REFramework、CMake、Visual Studio 或 Python 第三方包。脚本不联网、不下载依赖，也不执行安装或上传。

## 命令

在解压得到的 `LifetimeGuard` 目录打开 PowerShell。将变量改为本机已解压工具链的 `bin` 目录；下列位置只是示例，不是脚本中的固定路径。

```powershell
$Toolchain = 'C:/Tools/llvm-mingw/bin'
python --version
& "$Toolchain/x86_64-w64-mingw32-clang++.exe" --version
python -B scripts/build.py --toolchain "$Toolchain" --out out/release
python -B scripts/package.py --build out/release
```

若 Python 没有加入 PATH，可用其可执行文件的完整路径替换命令中的 `python`。源码和工具链路径可包含空格。

发布版源码和含注释版源码使用相同命令。`Source_Beta` 仅表示保留中文函数注释的源码，不是另一种运行产品；不需要 `--variant` 参数，也不会生成 Beta 安装包。

## 工具输出与产物

对应工具版本输出的首行如下；编译器还会输出它在本机的安装路径。

```text
Python 3.12.14
clang version 22.1.8 (https://github.com/llvm/llvm-project.git ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)
```

成功构建输出包括 `compiler_version PASS`、五个 `compile_*.o PASS`、`compile_dll PASS`，最后输出 JSON：

```json
{
  "status": "BUILT",
  "version": "0.6.0",
  "dll_sha256": "9a4b934b0a62169881499b9bc1a58bc32b2d0e157ee6892da2d6d2a3603b5b28"
}
```

| 路径 | 内容 |
| --- | --- |
| `out/release/DStorageFileLifetimeGuard.dll` | 优化后的发布 DLL |
| `out/release/build.json` | 本次构建状态、编译器首行、源码与依赖内容哈希 |
| `out/release/*.o`、`*.log` | 本机构建中间文件及编译器输出 |
| `dist/LifetimeGuard_v0.6.0/` | 完整发布文件夹，不再套一层压缩包 |
| `dist/LifetimeGuard_v0.6.0/LifetimeGuard_v0.6.0.zip` | 可交给 Mod 管理器安装的 ZIP |
| `dist/LifetimeGuard_v0.6.0/LICENSE`、`licenses/` | 与安装 ZIP 一起分发的完整许可证 |

打包命令输出发布文件夹、安装 ZIP 的文件名、SHA-256、字节数及附带许可证的哈希。
安装 ZIP 仅包含 `reframework/`、`modinfo.ini` 和 `README.md`；许可证放在发布文件夹中，
与安装 ZIP 同级，保持原有 `LICENSE`、`licenses/` 路径和内容，不安装到游戏目录。
README 使用源目录文件原始字节，不重排、不改换行。

发布或转发时必须提供完整的 `dist/LifetimeGuard_v0.6.0/` 文件夹，不单独分发里面的安装 ZIP，
也不把许可证作为另一个可选下载。构建不生成外层压缩包。两种源码包仍保留各自完整许可证。

## 编译和打包约定

生产代码保持单一 C++23 编译单元。C++ 使用 `-O2 -DNDEBUG -Wall -Wextra -Werror -static -DLG_LOOSE_CLEANUP -fms-extensions -shared`，链接器使用 `--no-insert-timestamp`。MinHook 以 C11 编译，启用固定入口后备、分配诊断以及 `0x7FFF0000ULL` 近地址范围。完整参数可直接查看 `scripts/build.py`。

编译按顺序执行，不启动并行编译池。每次构建先标记未完成，再编译本次源码快照；只有全部命令成功且输入未变化才写入成功状态。失败时查看对应日志，修正后重新运行同一构建命令。

`scripts/build-identity.json` 只记录版本、目标 DLL 哈希、源码输入和依赖输入哈希，是原版构建的内容标识，不是测试或实机报告。`package.py` 依据它拒绝未完成构建、过期输入或非原版 DLL，不依赖其他文件或外部工作区。

编译脚本允许编译修改后的源码；原版打包入口不会把修改后的 DLL 当作本版原始产物。派生版本应自行维护版本和构建标识，其正确性不由原版内容标识担保。

源码文本按 UTF-8 读取；第三方原文件不做注释或版权处理。ZIP 成员按文件名排序，使用固定时间、权限和 Deflate 压缩级别。发布打包环境为 Python 3.12.14、zlib 1.3.2。逐字节复现应使用相同 LLVM-MinGW 发行包及相同 Python/zlib 环境；其他工具链或压缩库版本不保证输出字节一致。
