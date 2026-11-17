"""Package annotated LifetimeGuard builds reproducibly; never deploy or upload."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile


ROOT = Path(__file__).resolve().parents[1]
DLL_NAME = "DStorageFileLifetimeGuard.dll"
WMP_DLL_NAME = "WMPLifetimeGuard.dll"
DLL_PATH = "reframework/plugins/"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def pack(path, files):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            if name.startswith("/") or ".." in Path(name).parts or "\\" in name:
                raise ValueError("Unsafe member: " + name)
            info = zipfile.ZipInfo(name, (2026, 9, 30, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(path) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())) or archive.testzip():
            raise ValueError("ZIP verification failed")
        if set(archive.namelist()) != set(files):
            raise ValueError("ZIP file list differs")
        for name, data in files.items():
            if archive.read(name) != data:
                raise ValueError("ZIP content differs: " + name)
    return dict(file=path.name, sha256=sha(path.read_bytes()), bytes=path.stat().st_size)


def source_files(directory):
    extensions = {".cpp", ".h", ".inl", ".lua"}
    return {path.relative_to(directory).as_posix(): path for path in sorted(directory.rglob("*"))
            if path.is_file() and path.suffix in extensions}


def bonesystem_files(root, version, *, wmp=False):
    source = root / "src/BoneSystem"
    bundle = "WMPOptimizationPatches" if wmp else "LifetimeGuard"
    entry = f"reframework/autorun/{bundle}"
    target = entry + "/BoneSystem/"
    profile = {
        "key": "__WMPOptimizationPackLookup" if wmp else "__BoneSystemLookupFix",
        "report_path": "WMPOptimizationPatches/BoneSystem.json" if wmp else "LifetimeGuard/BoneSystem.json",
        "lookup_path": target + "Lookup.lua",
        "version": version,
    }
    options = "".join(f"    {name} = {json.dumps(value)},\n" for name, value in profile.items())
    bootstrap = (source / "Bootstrap.lua").read_text("utf-8")
    return {
        target + "Bootstrap.lua": (
            "local install = (function()\n" + bootstrap + "end)()\n"
            "install(..., {\n" + options + "})\n"
        ).encode("utf-8"),
        target + "Lookup.lua": (source / "Lookup.lua").read_bytes(),
        (target + "Provider.lua" if wmp else entry + ".lua"):
            (source / ("WmpProvider.lua" if wmp else "Autorun.lua")).read_bytes(),
    }


def installer_files(root, dll, version):
    info = (f"name=LifetimeGuard v{version}\r\nversion={version}\r\nauthor=Laz\r\n"
            "description=File and texture lifetime protection\r\ncategory=Gameplay\r\n"
            "NameAsBundle=DirectStorage File Lifetime Guard v0.1\r\n").encode("utf-8")
    files = {
        DLL_PATH + DLL_NAME: dll,
        "README.md": (root / "README.md").read_bytes(),
        "modinfo.ini": info,
        "LICENSE": (root / "LICENSE").read_bytes(),
        **bonesystem_files(root, version),
    }
    for path in sorted((root / "licenses").iterdir()):
        if path.is_file():
            files["licenses/" + path.name] = path.read_bytes()
    return files


def verified_build(root, directory, *, wmp=False, require_tests=False):
    """Validate the binary, source snapshot and dependencies against the build receipt."""
    state = json.loads((directory / "build.json").read_text("utf-8"))
    if state["status"] != "BUILT" or any(call["returncode"] for call in state["calls"]):
        raise ValueError("Build did not finish successfully")
    if require_tests and not state["tests_passed"]:
        raise ValueError("Use a build that passed the isolated LifetimeGuard test suite")
    current = {name: sha(path.read_text("utf-8-sig").encode("utf-8"))
               for name, path in source_files(root / "src").items()}
    if current != state["source"]:
        raise ValueError("Current source changed after building")
    dependencies = json.loads((root / "third_party/manifest.json").read_text("utf-8"))
    if dependencies != state["dependencies"]:
        raise ValueError("Dependency manifest changed after building")
    for name, expected in dependencies.items():
        if sha((root / name).read_bytes()) != expected:
            raise ValueError("Dependency changed: " + name)
    dll_name = WMP_DLL_NAME if wmp else DLL_NAME
    dll = (directory / dll_name).read_bytes()
    if sha(dll) != state["binaries"][dll_name]:
        raise ValueError("DLL differs from the build receipt")
    return dll, state["version"]


def main(*, require_tests=False):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--out", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    directory = args.build.resolve()
    if not directory.is_relative_to(ROOT / "out"):
        raise ValueError("Use a build below this source tree's out directory")
    dll, version = verified_build(ROOT, directory, require_tests=require_tests)
    files = installer_files(ROOT, dll, version)
    output = args.out.resolve()
    record = pack(output / f"LifetimeGuard_v{version}.zip", files)
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
