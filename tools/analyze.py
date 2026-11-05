#!/usr/bin/env python3
"""Summarize PresentMon 2.x CSV captures by swapchain.

Usage:
    python tools/analyze.py captures/*.csv
    python tools/analyze.py --markdown captures/*.csv
    python tools/analyze.py --append docs/findings/002-pacing-baseline.md captures/*.csv

Reports, per file and per swapchain:
  * presented frame count and frames per second
  * MsBetweenDisplayChange avg / p50 / p95 / p99 / max / stddev
  * share of intervals above 1.5x the median (stutter)
  * share of intervals below 0.5x the median (bunching)
  * the same for MsBetweenPresents
  * optional latency columns present in the PresentMon 2.x v1 schema:
    MsUntilDisplayed, MsInPresentAPI, MsUntilRenderComplete, MsGPUActive
  * with --histogram, the output-interval distribution in fixed-width bins

The swapchain with the highest present rate is the frame-generation output;
the other one carries the game's real frames.

Latency caveat: MsUntilDisplayed is PresentMon's software estimate from the
Present call to the display change. It is a useful relative proxy for
presentation latency, but it is not click-to-photon: it excludes input
sampling, game simulation and the physical display response, and it is not
comparable across present modes or display pipelines. MsInPresentAPI measures
how long the application blocked inside Present (including Sedici's limiter
wait), so it is not latency either.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import math
import os
import statistics
import sys
from dataclasses import dataclass, field


def normalize(name: str) -> str:
    return "".join(ch for ch in name.strip().lower() if ch.isalnum())


def find_column(header: list[str], wanted: str) -> str | None:
    target = normalize(wanted)
    for name in header:
        if normalize(name) == target:
            return name
    return None


def to_float(text: str | None) -> float | None:
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    try:
        value = float(text)
    except ValueError:
        return None
    if math.isnan(value) or math.isinf(value):
        return None
    return value


def format_swapchain(text: str) -> str:
    """Normalize 0x... addresses so both PresentMon schemas group identically."""
    text = text.strip()
    if not text:
        return "unknown"
    try:
        return f"0x{int(text, 16):016X}"
    except ValueError:
        return text


@dataclass
class Series:
    count: int = 0
    avg: float = 0.0
    p50: float = 0.0
    p95: float = 0.0
    p99: float = 0.0
    maximum: float = 0.0
    sd: float = 0.0
    above: float = 0.0
    below: float = 0.0
    notes: str = ""


def summarize(values: list[float]) -> Series:
    if not values:
        return Series()
    ordered = sorted(values)
    median = statistics.median(ordered)
    n = len(ordered)

    def percentile(fraction: float) -> float:
        index = max(1, math.ceil(fraction * n))
        return ordered[min(index, n) - 1]

    series = Series()
    series.count = n
    series.avg = statistics.fmean(ordered)
    series.p50 = median
    series.p95 = percentile(0.95)
    series.p99 = percentile(0.99)
    series.maximum = ordered[-1]
    series.sd = statistics.pstdev(ordered) if n > 1 else 0.0
    if median > 0:
        series.above = 100.0 * sum(1 for v in ordered if v > 1.5 * median) / n
        series.below = 100.0 * sum(1 for v in ordered if v < 0.5 * median) / n
    return series


@dataclass
class SwapChainStats:
    swapchain: str
    display: Series
    presents: Series
    duration_s: float = 0.0
    fps: float = 0.0
    until_displayed: Series = field(default_factory=Series)
    in_present_api: Series = field(default_factory=Series)
    until_render_complete: Series = field(default_factory=Series)
    gpu_active: Series = field(default_factory=Series)
    display_values: list[float] = field(default_factory=list)


@dataclass
class FileStats:
    path: str
    label: str
    swapchains: list[SwapChainStats] = field(default_factory=list)
    error: str = ""


def analyze_file(path: str) -> FileStats:
    result = FileStats(path=path, label=os.path.basename(path))
    try:
        with open(path, "r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.reader(handle)
            try:
                header = next(reader)
            except StopIteration:
                result.error = "empty file"
                return result

            display_column = find_column(header, "MsBetweenDisplayChange")
            presents_column = find_column(header, "MsBetweenPresents")
            if not presents_column:
                # PresentMon 2.3+ frame-time schema, if --v1_metrics was omitted.
                presents_column = find_column(header, "FrameTime")
            if not display_column:
                display_column = find_column(header, "DisplayedTime")
            swapchain_column = find_column(header, "SwapChainAddress")

            # Optional PresentMon 2.x latency columns. Older schemas simply do
            # not have them; the tables are then omitted.
            latency_columns = {
                "until_displayed": find_column(header, "MsUntilDisplayed"),
                "in_present_api": find_column(header, "MsInPresentAPI"),
                "until_render_complete": find_column(header, "MsUntilRenderComplete"),
                "gpu_active": find_column(header, "MsGPUActive"),
            }

            if not presents_column:
                result.error = "no MsBetweenPresents / msBetweenPresents column"
                return result

            def index(name: str | None) -> int | None:
                return header.index(name) if name else None

            i_display = index(display_column)
            i_presents = index(presents_column)
            i_swap = index(swapchain_column)
            i_latency = {key: index(name) for key, name in latency_columns.items()}

            display: dict[str, list[float]] = {}
            presents: dict[str, list[float]] = {}
            latency: dict[str, dict[str, list[float]]] = {key: {} for key in latency_columns}

            for row in reader:
                if not row:
                    continue
                raw_swapchain = row[i_swap].strip() if i_swap is not None and i_swap < len(row) else ""
                swapchain = format_swapchain(raw_swapchain)
                presents.setdefault(swapchain, [])
                display.setdefault(swapchain, [])

                value = to_float(row[i_presents]) if i_presents < len(row) else None
                if value is not None and value >= 0:
                    presents[swapchain].append(value)

                if i_display is not None and i_display < len(row):
                    value = to_float(row[i_display])
                    if value is not None and value >= 0:
                        display[swapchain].append(value)

                for key, i_column in i_latency.items():
                    latency[key].setdefault(swapchain, [])
                    if i_column is not None and i_column < len(row):
                        value = to_float(row[i_column])
                        if value is not None and value >= 0:
                            latency[key][swapchain].append(value)

        for swapchain in sorted(presents, key=lambda key: -len(presents[key])):
            entry = SwapChainStats(
                swapchain=swapchain,
                display=summarize(display.get(swapchain, [])),
                presents=summarize(presents.get(swapchain, [])),
            )
            entry.until_displayed = summarize(latency["until_displayed"].get(swapchain, []))
            entry.in_present_api = summarize(latency["in_present_api"].get(swapchain, []))
            entry.until_render_complete = summarize(
                latency["until_render_complete"].get(swapchain, []))
            entry.gpu_active = summarize(latency["gpu_active"].get(swapchain, []))
            entry.display_values = display.get(swapchain, [])
            # Faster and schema independent than any timestamp column: the mean
            # interval between present calls is the frame period.
            if entry.presents.avg > 0:
                entry.fps = 1000.0 / entry.presents.avg
                entry.duration_s = entry.presents.avg * entry.presents.count / 1000.0
            result.swapchains.append(entry)
    except OSError as exc:
        result.error = f"cannot read: {exc}"
    return result


def order_swapchains(entry: FileStats) -> list[SwapChainStats]:
    return sorted(entry.swapchains, key=lambda item: -item.fps)


HEADERS = [
    "file",
    "swapchain",
    "n",
    "fps",
    "avg",
    "p50",
    "p95",
    "p99",
    "max",
    "sd",
    "%>1.5med",
    "%<0.5med",
]


def row_values(label: str, item: SwapChainStats, series: Series, role: str) -> list[str]:
    return [
        label,
        f"{item.swapchain}",
        str(series.count),
        f"{item.fps:.1f}",
        f"{series.avg:.3f}",
        f"{series.p50:.3f}",
        f"{series.p95:.3f}",
        f"{series.p99:.3f}",
        f"{series.maximum:.3f}",
        f"{series.sd:.3f}",
        f"{series.above:.2f}",
        f"{series.below:.2f}",
    ]


def print_rows(title: str, headers: list[str], rows: list[list[str]], markdown: bool) -> list[str]:
    out: list[str] = []
    out.append(title)
    if not rows:
        out.append("  (no data)")
        out.append("")
        return out

    if markdown:
        out.append("| " + " | ".join(headers) + " |")
        out.append("|" + "|".join(["---"] * len(headers)) + "|")
        for row in rows:
            out.append("| " + " | ".join(row) + " |")
        out.append("")
        return out

    widths = [len(name) for name in headers]
    for row in rows:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))

    def line(row: list[str]) -> str:
        return "  ".join(value.rjust(widths[index]) for index, value in enumerate(row))

    out.append(line(headers))
    out.append("  ".join("-" * width for width in widths))
    for row in rows:
        out.append(line(row))
    out.append("")
    return out


def print_table(title: str, rows: list[list[str]], markdown: bool) -> list[str]:
    return print_rows(title, HEADERS, rows, markdown)


def interval_histogram(values: list[float], bin_ms: float,
                       max_ms: float = 0.0) -> list[tuple[float, float, int, float]]:
    """Fixed-width bins starting at the floor of the minimum value.

    With `max_ms > 0`, bins stop at `max_ms` and everything at or above it is
    collected in a final overflow bin. That keeps one 120 ms hitch from turning
    a 1 ms histogram into a 120-row table.
    """
    if not values or bin_ms <= 0:
        return []
    low = math.floor(min(values) / bin_ms) * bin_ms
    counts: dict[int, int] = {}
    overflow = 0
    for value in values:
        if max_ms > 0 and value >= max_ms:
            overflow += 1
            continue
        bucket = int(math.floor((value - low) / bin_ms))
        counts[bucket] = counts.get(bucket, 0) + 1
    total = len(values)
    last = max(counts) if counts else -1
    if max_ms > 0:
        last = max(last, int(math.ceil((max_ms - low) / bin_ms)) - 1)
    out = [
        (low + i * bin_ms, low + (i + 1) * bin_ms, counts.get(i, 0),
         100.0 * counts.get(i, 0) / total)
        for i in range(last + 1)
    ]
    if overflow:
        out.append((max_ms, float("inf"), overflow, 100.0 * overflow / total))
    return out


def build_report(files: list[FileStats], markdown: bool, histogram: bool = False,
                 bin_ms: float = 1.0, histogram_max_ms: float = 0.0) -> str:
    display_rows: list[list[str]] = []
    present_rows: list[list[str]] = []
    notes: list[str] = []

    for entry in files:
        if entry.error:
            notes.append(f"  {entry.label}: {entry.error}")
            continue
        ordered = order_swapchains(entry)
        for index, item in enumerate(ordered):
            if index == 0:
                role = "fg"
            else:
                role = "real"
            display_rows.append(row_values(entry.label, item, item.display, role))
            present_rows.append(row_values(entry.label, item, item.presents, role))

        if len(ordered) >= 2:
            fast, slow = ordered[0], ordered[1]
            ratio = fast.fps / slow.fps if slow.fps > 0 else 0.0
            notes.append(
                f"  {entry.label}: fg swapchain {fast.swapchain} at {fast.fps:.1f} fps, "
                f"real swapchain {slow.swapchain} at {slow.fps:.1f} fps, ratio {ratio:.2f}x"
            )
        elif ordered:
            notes.append(f"  {entry.label}: only one swapchain ({ordered[0].swapchain})")

    out: list[str] = []
    out.extend(print_table("MsBetweenDisplayChange (output cadence, ms)", display_rows, markdown))
    out.extend(print_table("MsBetweenPresents (present call cadence, ms)", present_rows, markdown))

    # Optional latency columns. Only emit a table when the CSV actually carried
    # the column and at least one swapchain has samples.
    latency_tables = [
        ("MsUntilDisplayed (present-to-display proxy, ms)", "until_displayed"),
        ("MsInPresentAPI (time blocked inside Present, ms)", "in_present_api"),
        ("MsUntilRenderComplete (present-to-GPU-complete, ms)", "until_render_complete"),
        ("MsGPUActive (GPU busy time, ms)", "gpu_active"),
    ]
    for title, attribute in latency_tables:
        rows: list[list[str]] = []
        for entry in files:
            if entry.error:
                continue
            for item in order_swapchains(entry):
                series = getattr(item, attribute)
                if series.count:
                    rows.append(row_values(entry.label, item, series, ""))
        if rows:
            out.extend(print_table(title, rows, markdown))

    if any(getattr(item, "until_displayed").count
           for entry in files if not entry.error
           for item in order_swapchains(entry)):
        out.append("Latency notes")
        out.append("  MsUntilDisplayed is PresentMon's software estimate from the Present call to")
        out.append("  the display change. Use it for relative comparisons only: it is not")
        out.append("  click-to-photon, it excludes input sampling and game simulation, and it is")
        out.append("  not comparable across present modes. MsInPresentAPI is how long the")
        out.append("  application blocked inside Present; with a limiter it includes the")
        out.append("  deliberate wait, so it is not latency. MsUntilRenderComplete reflects")
        out.append("  queue depth. MsGPUActive is GPU busy time per frame.")
        out.append("")

    if histogram:
        for entry in files:
            if entry.error:
                continue
            ordered = order_swapchains(entry)
            if not ordered:
                continue
            output = ordered[0]
            bins = interval_histogram(output.display_values, bin_ms, histogram_max_ms)
            if not bins:
                continue
            rows = []
            for low, high, count, pct in bins:
                label = f"{low:.1f}+" if math.isinf(high) else f"{low:.1f}-{high:.1f}"
                rows.append([label, str(count), f"{pct:.2f}"])
            title = (f"Output interval histogram, {bin_ms:g} ms bins "
                     f"(MsBetweenDisplayChange, {entry.label}, {output.swapchain})")
            out.extend(print_rows(title, ["bin (ms)", "frames", "%"], rows, markdown))

    if notes:
        out.append("Swapchains")
        out.extend(notes)
        out.append("")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Summarize PresentMon CSV captures by swapchain.")
    parser.add_argument("csv", nargs="+", help="PresentMon CSV files")
    parser.add_argument("--markdown", action="store_true", help="emit Markdown tables instead of fixed width text")
    parser.add_argument("--append", metavar="PATH", help="append the Markdown report to this file")
    parser.add_argument("--title", default="", help="heading used when appending")
    parser.add_argument("--histogram", action="store_true",
                        help="include the output-interval histogram")
    parser.add_argument("--bin-ms", type=float, default=1.0,
                        help="histogram bin width in milliseconds (default 1)")
    parser.add_argument("--histogram-max-ms", type=float, default=40.0,
                        help="stop the histogram at this value and add an overflow bin "
                             "(0 = no cap; default 40)")
    args = parser.parse_args(argv)

    if args.bin_ms <= 0:
        parser.error("--bin-ms must be greater than zero")
    if args.histogram_max_ms < 0:
        parser.error("--histogram-max-ms must be zero or greater")

    missing = [path for path in args.csv if not os.path.isfile(path)]
    for path in missing:
        print(f"missing: {path}", file=sys.stderr)
    files = [analyze_file(path) for path in args.csv if os.path.isfile(path)]
    if not files:
        return 1

    report = build_report(files, args.markdown or bool(args.append), args.histogram,
                          args.bin_ms, args.histogram_max_ms)
    print(report)

    if args.append:
        title = args.title or f"Capture summary {datetime.datetime.now():%Y-%m-%d %H:%M}"
        with open(args.append, "a", encoding="utf-8", newline="\n") as handle:
            handle.write(f"\n### {title}\n\n")
            handle.write(report.rstrip() + "\n")
        print(f"appended to {args.append}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
