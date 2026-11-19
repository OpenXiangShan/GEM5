"""Run the prebuilt two-hart corpus with the upstream XS-GEM5 checker."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from check import check_observations  # litmus-tests-riscv/platforms/xs-gem5


def run_image(gem5, corpus, failures, image, model, topology):
    with tempfile.TemporaryDirectory(prefix="gem5-litmus-") as work:
        log = Path(work) / "sim.log"
        command = [
            gem5,
            "--listener-mode=off",
            f"--outdir={work}",
            "configs/example/smt_idealkmhv3.py",
            "--raw-cpt",
            f"--generic-rv-cpt={corpus / image}",
            "--disable-difftest",
            "--mem-type=SimpleMemory",
            "--mem-size=16GB",
            "--maxinsts=5000000",
            "--abs-max-tick=10000000000",
        ]
        if topology == "spmt":
            command.append("--enable-spmt")
        try:
            with log.open("w") as stream:
                process = subprocess.run(
                    command,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    timeout=120,
                )
            text = log.read_text(errors="replace")
            if (
                process.returncode
                or "because m5_exit instruction encountered" not in text
            ):
                reason = f"incomplete simulation (exit {process.returncode})"
            else:
                result = check_observations(text, model, 32)
                if result["status"] == "pass":
                    return True
                reason = str(result)
        except (subprocess.TimeoutExpired, ValueError) as error:
            reason = str(error)
        dest = failures / topology / Path(image).with_suffix(".log")
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(log, dest)
        print(f"FAIL {topology} {image}: {reason}", flush=True)
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gem5", type=Path)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("--jobs", type=int, default=64)
    parser.add_argument(
        "--failures", type=Path, default=Path("rvwmo-failures")
    )
    args = parser.parse_args()
    images = json.loads((args.corpus / "manifest.json").read_text())
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [
            pool.submit(
                run_image,
                args.gem5,
                args.corpus,
                args.failures,
                image,
                model,
                topology,
            )
            for image, model in images.items()
            for topology in ("smt", "spmt")
        ]
        passed = sum(future.result() for future in futures)
    print(f"{passed}/{len(futures)} RVWMO simulations passed", flush=True)
    return 0 if futures and passed == len(futures) else 1


if __name__ == "__main__":
    raise SystemExit(main())
