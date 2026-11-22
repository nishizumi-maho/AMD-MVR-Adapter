"""Summarize benchmark runs captured by capture.ps1 into a Markdown table for the PR.

Usage: python -I analyze.py [runs_dir]   (default: bench/runs) -> prints Markdown and writes runs_dir/report.md
"""

import csv
import json
import statistics
import sys
from pathlib import Path


def read_csv(path):
    if not path.exists():
        return []
    with open(path, newline="") as file:
        return list(csv.DictReader(file))


def column(rows, *names):
    """Float values of the first column that exists (PresentMon 1.x/2.x use different names)."""
    for name in names:
        if rows and name in rows[0]:
            values = []
            for row in rows:
                try:
                    # counters.csv is written by PowerShell with the system locale (pt-BR: "20,6").
                    values.append(float(row[name].replace(",", ".")))
                except (TypeError, ValueError):
                    pass
            return values
    return []


def low_fps(frame_times_ms, fraction):
    """FPS of the slowest `fraction` of frames (e.g. 0.01 for the "1% low").

    Percentile definition (as PresentMon/CapFrameX "P1"): the frame time at percentile (1 - fraction), as FPS. With
    few frames, the index is clamped to the slowest frame.
    """
    ordered = sorted(frame_times_ms)
    index = min(len(ordered) - 1, int(len(ordered) * (1.0 - fraction)))
    return 1000.0 / ordered[index]


def mean(values):
    return statistics.fmean(values) if values else float("nan")


def summarize(run_dir):
    meta = json.loads((run_dir / "meta.json").read_text())
    frames = read_csv(run_dir / "presentmon.csv")
    counters = read_csv(run_dir / "counters.csv")
    irsdk = read_csv(run_dir / "irsdk.csv")

    frame_times = column(frames, "FrameTime", "MsBetweenPresents")
    return {
        "label": meta["label"],
        "frames": len(frame_times),
        "avg_fps": 1000.0 / mean(frame_times) if frame_times else float("nan"),
        "low1_fps": low_fps(frame_times, 0.01) if frame_times else float("nan"),
        "low01_fps": low_fps(frame_times, 0.001) if frame_times else float("nan"),
        "cpu_busy_ms": mean(column(frames, "MsCPUBusy", "CPUBusy")),
        "gpu_busy_ms": mean(column(frames, "MsGPUBusy", "GPUBusy", "MsGPUActive")),
        "cpu_total": mean(column(counters, "cpu_total")),
        "cpu_max_core": mean(column(counters, "cpu_max_core")),
        "iracing_cpu": mean(column(counters, "iracing_cpu")),
        "gpu_3d": mean(column(counters, "iracing_gpu_3d")),
        "ir_fps": mean(column(irsdk, "FrameRate")),
        "ir_cpu_fg": 100 * mean(column(irsdk, "CpuUsageFG")),
        "ir_gpu": 100 * mean(column(irsdk, "GpuUsage")),
        "meta": meta,
    }


ROWS = [
    ("Frames captured", "frames", "{:.0f}"),
    ("Average FPS (PresentMon)", "avg_fps", "{:.1f}"),
    ("1% low FPS", "low1_fps", "{:.1f}"),
    ("0.1% low FPS", "low01_fps", "{:.1f}"),
    ("CPU busy per frame (ms)", "cpu_busy_ms", "{:.2f}"),
    ("GPU busy per frame (ms)", "gpu_busy_ms", "{:.2f}"),
    ("CPU total (%)", "cpu_total", "{:.1f}"),
    ("Busiest CPU core (%)", "cpu_max_core", "{:.1f}"),
    ("iRacing process CPU (% of all cores)", "iracing_cpu", "{:.1f}"),
    ("iRacing GPU 3D engine (%)", "gpu_3d", "{:.1f}"),
    ("iRacing FrameRate (telemetry)", "ir_fps", "{:.1f}"),
    ("iRacing CpuUsageFG (telemetry, %)", "ir_cpu_fg", "{:.1f}"),
    ("iRacing GpuUsage (telemetry, %)", "ir_gpu", "{:.1f}"),
]


def main():
    runs_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "runs"
    runs = [summarize(d) for d in sorted(runs_dir.iterdir()) if (d / "meta.json").exists()]
    if not runs:
        print(f"No runs in {runs_dir}")
        return 1

    # Group the repetitions of a configuration ("box_3proj_r1", "box_3proj_r2", ... -> "box_3proj").
    configs = {}
    for run in runs:
        configs.setdefault(run["label"].rsplit("_r", 1)[0], []).append(run)

    def cell(group, key, fmt):
        values = [r[key] for r in group]
        if len(values) == 1:
            return fmt.format(values[0])
        if key == "frames":
            return fmt.format(sum(values))
        return f"{fmt.format(statistics.fmean(values))} ({fmt.format(min(values))}–{fmt.format(max(values))})"

    lines = ["| Metric | " + " | ".join(f"{name} (n={len(group)})" for name, group in configs.items()) + " |",
             "|---|" + "---:|" * len(configs)]
    for title, key, fmt in ROWS:
        lines.append(f"| {title} | " + " | ".join(cell(group, key, fmt) for group in configs.values()) + " |")
    meta = runs[0]["meta"]
    lines += ["", f"Mean of n repetitions (min–max). System: {meta['cpu']}, {meta['gpu']} (driver {meta['driver']}), "
                  f"{meta['seconds']} s per repetition."]

    report = "\n".join(lines)
    print(report)
    (runs_dir / "report.md").write_text(report + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
