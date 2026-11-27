"""Check clock profiles, legacy routing, endpoint counting and UART sampling using VCS."""
import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PROFILES = ("sync", "core2_noc1", "core1_noc2", "core3_noc2", "phase90")


def execute(argv, directory, logfile):
    with logfile.open("w") as stream:
        return subprocess.run(argv, cwd=directory, stdout=stream,
                              stderr=subprocess.STDOUT, timeout=120).returncode


def check(out, vcs):
    out.mkdir(parents=True, exist_ok=True)
    (out / "DifftestMacros.v").write_text("`define CONFIG_DIFFTEST_STEPWIDTH 1\n")
    vsrc = ROOT / "src/test/vsrc"
    for enabled in (True, False):
        work = out / ("profiles" if enabled else "legacy")
        work.mkdir(exist_ok=True)
        cmd = [vcs, "-full64", "-sverilog", "-timescale=1ns/1ps",
               "-top", "it_clock_profiles_tb", "+define+TB_NO_DPIC", "+incdir+" + str(out),
               str(vsrc / "st/ITClockGen.sv"), str(vsrc / "st/SystemExtensionWrapper.sv"),
               str(vsrc / "vcs/DifftestEndpoint.sv"), str(ROOT / "scripts/tests/it_clock_profiles_tb.sv"),
               "-o", str(work / "simv")]
        if enabled:
            cmd.append("+define+IT_CLOCK_CONFIG")
        assert execute(cmd, work, work / "compile.log") == 0, work / "compile.log"
        for profile in PROFILES if enabled else ("sync",):
            log = work / (profile + ".log")
            rc = execute([str(work / "simv"), "-no_save", "+IT_CLOCK_PROFILE=" + profile], work, log)
            text = log.read_text()
            assert rc == 0 and "CLOCK_COMPONENT_PASS" in text, log
            assert text.count("ITUART!\n") == 1, log
            print("PASS", work.name, profile)
        if enabled:
            log = work / "invalid.log"
            execute([str(work / "simv"), "-no_save", "+IT_CLOCK_PROFILE=invalid"], work, log)
            text = log.read_text()
            # Some VCS versions return zero even for $fatal.
            assert "IT_CLOCK_CONFIG unknown profile" in text and "CLOCK_COMPONENT_PASS" not in text, log
            print("PASS invalid profile rejected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--vcs", default="vcs")
    args = parser.parse_args()
    if args.out:
        check(args.out.resolve(), args.vcs)
    else:
        with tempfile.TemporaryDirectory(prefix="it-clock-profiles-") as directory:
            check(Path(directory), args.vcs)


if __name__ == "__main__":
    main()
