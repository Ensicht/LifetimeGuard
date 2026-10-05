"""Reproduce the release folder and inner installer, without installing or uploading."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

try:
    import zlib
except ImportError as error:
    raise RuntimeError("Use a complete Python distribution with the standard zlib module") from error

ROOT = Path(__file__).resolve().parents[1]
DLL_NAME = "DStorageFileLifetimeGuard.dll"
SOURCE_SUFFIXES = {".cpp", ".h", ".inl"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def source_files(root):
    return {p.relative_to(root / "src").as_posix(): p.read_text("utf-8-sig").encode("utf-8")
            for p in sorted((root / "src").rglob("*"))
            if p.is_file() and p.suffix in SOURCE_SUFFIXES}


def source_manifest(root):
    return {name: sha(data) for name, data in source_files(root).items()}


def version(root):
    text = (root / "src/GuardBuild.h").read_text("utf-8-sig")
    match = re.search(r'^#define LG_BUILD_TEXT "(\d+\.\d+\.\d+)"$', text, re.M)
    if not match:
        raise ValueError("Expected a numeric release version in GuardBuild.h")
    return match.group(1)


def dependencies(root):
    manifest = json.loads((root / "third_party/manifest.json").read_text("utf-8"))
    for relative, expected in manifest.items():
        target = (root / relative).resolve()
        if not target.is_relative_to((root / "third_party").resolve()):
            raise ValueError("Dependency is outside third_party: " + relative)
        if sha(target.read_bytes()) != expected:
            raise ValueError("Dependency changed: " + relative)
    return manifest


def output_directory(root, directory, base):
    directory = directory.resolve()
    boundary = (root / base).resolve()
    if not boundary.is_relative_to(root.resolve()) or not directory.is_relative_to(boundary):
        raise ValueError("Output must be beneath this source tree's " + base + " directory")
    return directory


def pack(path, files):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w") as archive:
        for name, data in sorted(files.items()):
            if name.startswith("/") or ".." in name.split("/") or "\\" in name or ":" in name:
                raise ValueError("Unsafe ZIP member: " + name)
            info = zipfile.ZipInfo(name, (2026, 9, 30, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o600 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=9)
    with zipfile.ZipFile(temporary) as archive:
        if archive.testzip() or archive.namelist() != sorted(files):
            raise ValueError("ZIP inventory verification failed")
        for name, data in files.items():
            if archive.read(name) != data:
                raise ValueError("ZIP content differs: " + name)
    temporary.replace(path)
    return dict(file=path.name, sha256=sha(path.read_bytes()), bytes=path.stat().st_size)


def installer_files(root, dll, release_version):
    info = (f"name=LifetimeGuard v{release_version}\r\nversion={release_version}\r\nauthor=Laz\r\n"
            "description=File and texture lifetime protection\r\ncategory=Gameplay\r\n"
            "NameAsBundle=DirectStorage File Lifetime Guard v0.1\r\n").encode("utf-8")
    files = {"reframework/plugins/" + DLL_NAME: dll,
             "README.md": (root / "README.md").read_bytes(),
             "modinfo.ini": info}
    return files


def license_files(root):
    files = {"LICENSE": (root / "LICENSE").read_bytes()}
    for name in ("MinHook.txt", "REFramework.txt", "LLVM.txt",
                 "COPYING.winpthreads.txt", "COPYING.MinGW-w64-runtime.txt"):
        files["licenses/" + name] = (root / "licenses" / name).read_bytes()
    return files


def release_folder(root, dist, dll, release_version):
    """Keep required notices beside the installable ZIP in one complete folder."""
    files = installer_files(root, dll, release_version)
    notices = license_files(root)
    name = f"LifetimeGuard_v{release_version}"
    folder = dist / name
    if not folder.resolve().is_relative_to(dist.resolve()):
        raise ValueError("Release folder is outside dist")
    allowed = set(notices) | {name + ".zip"}
    for path in folder.rglob("*"):
        if path.is_symlink() or (path.is_file() and path.relative_to(folder).as_posix() not in allowed):
            raise ValueError("Use a clean release folder; unexpected entry: " + str(path))
    folder.mkdir(parents=True, exist_ok=True)
    for relative, data in notices.items():
        target = folder / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    record = pack(folder / (name + ".zip"), files)
    record["release_folder"] = name
    record["accompanying_files"] = {n: sha(d) for n, d in sorted(notices.items())}
    return record


def verified_build(root, directory):
    identity = json.loads((root / "scripts/build-identity.json").read_text("utf-8"))
    state = json.loads((directory / "build.json").read_text("utf-8"))
    if state.get("status") != "BUILT":
        raise ValueError("Build has not completed successfully")
    if version(root) != identity["version"] or state["version"] != identity["version"]:
        raise ValueError("Release version differs")
    current = source_manifest(root)
    if current != state["source"] or current != identity["source"]:
        raise ValueError("Source differs from the compiled release inputs")
    deps = dependencies(root)
    if deps != state["dependencies"] or deps != identity["dependencies"]:
        raise ValueError("Dependency inputs differ")
    dll = (directory / DLL_NAME).read_bytes()
    if sha(dll) != state["dll_sha256"] or sha(dll) != identity["dll_sha256"]:
        raise ValueError("DLL differs from the pinned release build")
    return dll, identity["version"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "out/release")
    args = parser.parse_args()
    directory = output_directory(ROOT, args.build, "out")
    dll, release_version = verified_build(ROOT, directory)
    dist = output_directory(ROOT, ROOT / "dist", "dist")
    record = release_folder(ROOT, dist, dll, release_version)
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
