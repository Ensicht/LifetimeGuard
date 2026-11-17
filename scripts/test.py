"""Run regression/fault injection in independent processes, never in the game."""
import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    out = args.build.resolve()
    if not out.is_relative_to(ROOT / "out"):
        raise ValueError("Tests must run beneath the repository out directory")
    (out / "tests.json").write_text(json.dumps(dict(passed=False, status="RUNNING", version=args.version)), encoding="utf-8")
    evidence = out / "test_evidence"
    evidence.mkdir(exist_ok=True)
    results = []

    def run(name, *arguments, count=1):
        for attempt in range(count):
            executable = out / (name + ".exe")
            command = [str(executable), *map(str, arguments)]
            proc = subprocess.run(command, cwd=out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            label = re.sub(r"[^A-Za-z0-9_.-]", "_", name + "_" + "_".join(map(str, arguments)) + "_" + str(attempt))
            (evidence / (label + ".txt")).write_bytes(proc.stdout)
            results.append(dict(test=name, arguments=list(map(str, arguments)), attempt=attempt, returncode=proc.returncode))
            if proc.returncode:
                raise RuntimeError(f"{label} failed ({proc.returncode}): " + proc.stdout.decode("utf-8", errors="replace"))
        print(f"{name} {arguments} repetitions={count} PASS", flush=True)

    run("DStorageLooseCleanupTest", count=3)
    run("DStorageDetachedTextureProtocolTest", count=3)
    for mode in [(), ("--guard",), ("--guard-late",), ("--guard-bypass",), ("--guard-drain",)]:
        run("DStorageFileLifetimeGuardHarness", *mode)
    repeats = {
        "DStorageLoadingGateSimulation": 20, "DStoragePostPlayerEndProtectionSimulation": 5,
        "DStorageFileLifetimeGuardStress": 10, "DStorageLifecycleIntegrationHarness": 10,
        "DStorageResourceRecoverySimulation": 3, "DStorageResourceRecoveryHookHarness": 3,
        "DStorageResourceRecoveryNegativeHost": 3, "DStoragePointerHookTransactionTest": 3,
        "DStorageAllocationDiagnosticThreadTest": 10, "DStorageStatusUiTest": 3,
        "DStorageDetachedTextureDllHarness": 10, "DStorageDetachedTextureBoundaryTest": 10,
        "DStorageGameplayTextureRetryTest": 10, "DStorageFailurePolicyTest": 3,
        "DStorageFixed16Harness": 5,
    }
    for name, count in repeats.items():
        run(name, count=count)
    run("DStorageMinHookNearAllocationLegacy", "--expect-failure", count=3)
    run("DStorageMinHookNearAllocationFixed", "--expect-success", count=10)
    run("DStorageResourceSignatureVerifier", args.game.resolve() / "MonsterHunterWilds.exe")
    for mode in ["success", "create-fail", "delete-conflict", "ready-conflict", "protect-fail", "enable-fail"]:
        run("DStorageDetachedNativeInstallHarness", mode, count=10 if mode == "success" else 3)
    run("DStorageDetachedNativeInstallHarness", "profile", args.game.resolve() / "MonsterHunterWilds.exe", count=3)
    for mode in ["fallback", "conflict", "ambiguous", "missing-worker", "missing-submit",
                 "prehook", "prehook-bad-body", "prehook-bad-jump", "prehook-self-jump"]:
        run("DStorageResourceRecoveryHookHarness", "--vtable-" + mode, count=10 if mode in {"fallback", "prehook"} else 3)
        text = (out / "reframework/data/DStorageFileLifetimeGuard/events.log").read_text("utf-8")
        required = [r"stage=mh_create .*status=9 status_name=MH_ERROR_MEMORY_ALLOC",
                    r"hook_allocator .*free_candidates=0 attempts=0 .*query_error=0 allocation_error=0"]
        if mode in {"fallback", "prehook"}:
            required.append(r"stage=pointer_install .*status=0 status_name=installed")
        if mode == "prehook":
            required.append(r"stage=validate_task_submit .*status=2 status_name=compatible_rel32_entry")
        if not all(re.search(pattern, text) for pattern in required):
            raise AssertionError("Missing fallback evidence: " + mode)
        (evidence / ("vtable_" + mode + ".log")).write_text(text, encoding="utf-8")
    for mode in ["far-success", "far-create-fail", "far-prefix-conflict", "far-delete-conflict",
                 "far-ready-conflict", "far-protect-fail", "far-enable-conflict"]:
        run("DStorageDetachedNativeInstallHarness", mode)
        log = out / "reframework/data/DStorageFileLifetimeGuard/events.log"
        shutil.copy2(log, evidence / (mode + ".log"))
        if mode == "far-success":
            text = log.read_text("utf-8")
            if not re.search(r"component=texture_retry stage=mh_create index=0 .*status=9", text) or not re.search(r"component=texture_retry stage=mh_create index=1 .*status=0", text):
                raise AssertionError("Missing near-failure/far-success evidence")
    for script, extra in [("Test-DStorageInstallLogHistory.ps1", []),
                          ("Test-DStorageFailureReports.ps1", ["-ExpectedVersion", args.version])]:
        # Windows PowerShell -File can exit during startup without running the script.
        # Invoke the same script explicitly; literal quoting also supports spaces/apostrophes.
        def quote(value):
            return "'" + str(value).replace("'", "''") + "'"

        command = "& " + quote(ROOT / "tests" / script) + " -BuildDir " + quote(out)
        if extra:
            command += " -ExpectedVersion " + quote(args.version)
        proc = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
                              cwd=out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (evidence / (script + ".txt")).write_bytes(proc.stdout)
        results.append(dict(test=script, returncode=proc.returncode))
        if proc.returncode:
            raise RuntimeError(f"{script} failed ({proc.returncode}): " + proc.stdout.decode("utf-8", errors="replace"))
    run("DStorageFileLifetimeGuardHarness")
    (out / "tests.json").write_text(json.dumps(dict(passed=True, version=args.version,
                                                   invocations=len(results), results=results), indent=2), encoding="utf-8")
    print(f"PASS: {len(results)} native regression invocations plus log/history assertions")


if __name__ == "__main__":
    main()
