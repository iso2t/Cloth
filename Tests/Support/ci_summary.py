# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from collections import Counter
from html import escape
import json
import os
from pathlib import Path
import sys
from xml.etree import ElementTree


def lit_results(path):
    with path.open(encoding="utf-8") as source:
        report = json.load(source)
    cases = [(case["code"], case["name"], float(case["elapsed"] or 0)) for case in report["tests"]]
    return float(report["elapsed"]), cases


def unit_results(path):
    report = ElementTree.parse(path).getroot()
    cases = []
    for case in report.iter("testcase"):
        if case.find("failure") is not None:
            outcome = "FAIL"
        elif case.find("error") is not None:
            outcome = "ERROR"
        elif case.get("status") == "notrun":
            outcome = "DISABLED"
        elif case.get("result") == "skipped":
            outcome = "SKIPPED"
        else:
            outcome = "PASS"
        name = f"{case.attrib['classname']}.{case.attrib['name']}"
        cases.append((outcome, name, float(case.attrib["time"])))
    return float(report.attrib["time"]), cases


def main(directory):
    lines = [
        "## Test results", "",
        "| Runner | Executed | Elapsed | Outcomes | Slowest case |",
        "| --- | ---: | ---: | --- | --- |",
    ]
    errors = []
    for runner, filename, read in (("LLVM lit", "lit.json", lit_results), ("GoogleTest", "unit.xml", unit_results)):
        try:
            elapsed, cases = read(directory / filename)
        except (OSError, ValueError, KeyError, TypeError, ElementTree.ParseError) as error:
            errors.append(f"Could not read {filename} ({error})")
            lines.append(f"| {runner} | — | — | Report unavailable | — |")
            continue

        counts = Counter(outcome for outcome, _, _ in cases)
        outcomes = ", ".join(f"{count} {outcome}" for outcome, count in sorted(counts.items()))
        executed = [
            (seconds, name) for outcome, name, seconds in cases
            if outcome not in {"EXCLUDED", "UNSUPPORTED", "DISABLED", "SKIPPED"}
        ]
        slowest = "—"
        if executed:
            seconds, name = max(executed)
            label = escape(name).replace("|", "&#124;")
            slowest = f"<code>{label}</code> ({seconds:.3f} s)"
        else:
            errors.append(f"{runner} ran zero cases.")
        lines.append(f"| {runner} | {len(executed)} | {elapsed:.3f} s | {outcomes or '—'} | {slowest} |")

    if errors:
        lines.extend(["", *errors])
    summary = "\n".join(lines) + "\n"
    print(summary, end="")
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as output:
            output.write(summary)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1])))
