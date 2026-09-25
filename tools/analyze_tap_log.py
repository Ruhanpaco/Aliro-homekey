#!/usr/bin/env python3
"""Summarize Aliro HomeKey serial logs for tap/retry debugging."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

TX_OK = re.compile(
    r"transaction ok \((fast|standard)\) in (\d+) ms",
    re.IGNORECASE,
)
TX_FAIL = re.compile(
    r"transaction failed after (\d+) ms:\s*(.+?)\s*$",
    re.IGNORECASE,
)
APDU_FAIL = re.compile(
    r"APDU exchange attempt (\d+)/(\d+) failed:\s*(.+?)\s*$",
    re.IGNORECASE,
)
APDU_RECOVER = re.compile(
    r"APDU exchange recovered on attempt (\d+)/(\d+)",
    re.IGNORECASE,
)
LOG_HEADER = re.compile(
    r"^\s*([EWIDV])(?:\s+\(\d+\))?\s+([^:]+):",
    re.IGNORECASE,
)


@dataclass
class Summary:
    transactions: int
    successful: int
    failed: int
    success_rate: float | None
    fast: int
    standard: int
    median_duration_ms: float | None
    p95_duration_ms: int | None
    failure_reasons: dict[str, int]
    apdu_failed_attempts: int
    apdu_recoveries: int
    apdu_failure_reasons: dict[str, int]
    pn532_error_lines: int


def _p95(values: list[int]) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return ordered[index]


def summarize(lines: Iterable[str]) -> Summary:
    successful = 0
    failed = 0
    fast = 0
    standard = 0
    durations: list[int] = []
    failure_reasons: Counter[str] = Counter()
    apdu_failure_reasons: Counter[str] = Counter()
    apdu_failed_attempts = 0
    apdu_recoveries = 0
    pn532_error_lines = 0

    for line in lines:
        ok = TX_OK.search(line)
        if ok:
            successful += 1
            fast += ok.group(1).lower() == "fast"
            standard += ok.group(1).lower() == "standard"
            durations.append(int(ok.group(2)))
            continue

        failed_tx = TX_FAIL.search(line)
        if failed_tx:
            failed += 1
            durations.append(int(failed_tx.group(1)))
            failure_reasons[failed_tx.group(2).strip()] += 1
            continue

        apdu_failed = APDU_FAIL.search(line)
        if apdu_failed:
            apdu_failed_attempts += 1
            apdu_failure_reasons[apdu_failed.group(3).strip()] += 1

        if APDU_RECOVER.search(line):
            apdu_recoveries += 1

        header = LOG_HEADER.match(line)
        if (
            header
            and header.group(1).upper() in {"E", "W"}
            and header.group(2).strip().lower() == "nfc/pn532"
        ):
            pn532_error_lines += 1

    transactions = successful + failed
    return Summary(
        transactions=transactions,
        successful=successful,
        failed=failed,
        success_rate=(successful / transactions * 100.0) if transactions else None,
        fast=fast,
        standard=standard,
        median_duration_ms=statistics.median(durations) if durations else None,
        p95_duration_ms=_p95(durations),
        failure_reasons=dict(failure_reasons.most_common()),
        apdu_failed_attempts=apdu_failed_attempts,
        apdu_recoveries=apdu_recoveries,
        apdu_failure_reasons=dict(apdu_failure_reasons.most_common()),
        pn532_error_lines=pn532_error_lines,
    )


def _fmt_percent(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.1f}%"


def _fmt_duration(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float) and not value.is_integer():
        return f"{value:.1f} ms"
    return f"{int(value)} ms"


def render_text(summary: Summary) -> str:
    lines = [
        "Tap summary",
        "-----------",
        f"Transactions:       {summary.transactions}",
        f"Successful:         {summary.successful} ({_fmt_percent(summary.success_rate)})",
        f"Failed:             {summary.failed}",
        f"Fast:               {summary.fast}",
        f"Standard:           {summary.standard}",
        f"Median duration:    {_fmt_duration(summary.median_duration_ms)}",
        f"P95 duration:       {_fmt_duration(summary.p95_duration_ms)}",
        "",
        "Failures",
        "--------",
    ]

    if summary.failure_reasons:
        for reason, count in summary.failure_reasons.items():
            lines.append(f"{reason}: {count}")
    else:
        lines.append("None")

    lines.extend(
        [
            "",
            "APDU retries",
            "------------",
            f"Failed attempts:    {summary.apdu_failed_attempts}",
            f"Recovered exchanges: {summary.apdu_recoveries}",
        ]
    )

    if summary.apdu_failure_reasons:
        for reason, count in summary.apdu_failure_reasons.items():
            lines.append(f"{reason}: {count}")

    lines.extend(
        [
            "",
            "PN532",
            "-----",
            f"Warning/error lines: {summary.pn532_error_lines}",
        ]
    )
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Summarize Aliro HomeKey serial logs without extra dependencies."
    )
    parser.add_argument("logfile", type=Path, help="Serial log to analyse")
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the summary as JSON instead of text",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()

    try:
        with args.logfile.open("r", encoding="utf-8", errors="replace") as handle:
            summary = summarize(handle)
    except OSError as exc:
        raise SystemExit(f"cannot read {args.logfile}: {exc}") from exc

    if args.json:
        print(json.dumps(asdict(summary), indent=2, sort_keys=True))
    else:
        print(render_text(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
