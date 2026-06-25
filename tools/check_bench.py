#!/usr/bin/env python3
"""Compare JSONL benchmark artifacts produced on the same machine/configuration."""

import argparse
import json
import sys
from pathlib import Path


def load(path: Path) -> tuple[dict, dict[str, dict]]:
    header = None
    scenarios: dict[str, dict] = {}
    for line in path.read_text().splitlines():
        record = json.loads(line)
        if "scenario" in record:
            scenarios[record["scenario"]] = record
        elif "compiler" in record:
            header = record
    if header is None:
        raise ValueError(f"{path} has no benchmark header")
    return header, scenarios


def p99(record: dict) -> int:
    # Clock-overhead calibration is retained as diagnostic data, but its value can
    # vary by one clock tick between otherwise identical runs. Gate raw p99 to
    # compare the same measurement method across artifacts.
    return int(record["raw_p99_ns"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    baseline_header, baseline = load(args.baseline)
    candidate_header, candidate = load(args.candidate)
    for key in ("compiler", "cpp", "orders", "seed"):
        if baseline_header.get(key) != candidate_header.get(key):
            print(f"incompatible artifacts: {key} differs", file=sys.stderr)
            return 2

    failures: list[str] = []
    deep = "deep_wide_level_churn"
    if deep in baseline and deep in candidate:
        if p99(candidate[deep]) > p99(baseline[deep]) * 0.75:
            failures.append(f"{deep}: p99 must improve by at least 25%")
    else:
        print("note: deep/wide comparison skipped because one artifact predates the scenario", file=sys.stderr)

    for scenario, old in baseline.items():
        if scenario == deep or scenario not in candidate:
            continue
        if p99(candidate[scenario]) > p99(old) * 1.05:
            failures.append(f"{scenario}: p99 regressed by more than 5%")

    if failures:
        print("benchmark gate failed:", *failures, sep="\n", file=sys.stderr)
        return 1
    print("benchmark gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
