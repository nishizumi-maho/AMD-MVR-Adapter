"""Render the PR charts from bench/runs (box runs, 3 repetitions per configuration).

Usage: python -I charts.py <runs_dir> <output_dir>
"""

import csv
import statistics
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Configurations in display order: (run label prefix, display name, color). Categorical slots 1-3 of the validated
# palette, fixed per configuration across every chart.
CONFIGS = [
    ("box_baseline", "1 projection", "#2a78d6"),
    ("box_3proj", "3 projections", "#eb6834"),
    ("box_3proj_smp", "3 projections + SMP", "#1baf7a"),
]
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_2 = "#52514e"
GRID = "#e4e3df"

plt.rcParams.update({
    "font.family": "Segoe UI",
    "font.size": 11,
    "text.color": TEXT,
    "axes.labelcolor": TEXT_2,
    "axes.edgecolor": GRID,
    "xtick.color": TEXT_2,
    "ytick.color": TEXT_2,
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
})


def load_runs(runs_dir, prefix):
    """Per repetition: frame times (ms), CPU busy (ms) and GPU busy (ms) lists."""
    runs = []
    for run_dir in sorted(runs_dir.glob(prefix + "_r*")):
        with open(run_dir / "presentmon.csv", newline="") as file:
            rows = list(csv.DictReader(file))
        runs.append({
            "frame": [float(r["MsBetweenPresents"]) for r in rows],
            "cpu": [float(r["MsCPUBusy"]) for r in rows],
            "gpu": [float(r["MsGPUBusy"]) for r in rows],
        })
    return runs


def low_fps(frame_times, fraction=0.01):
    ordered = sorted(frame_times)
    return 1000.0 / ordered[min(len(ordered) - 1, int(len(ordered) * (1.0 - fraction)))]


def style_axis(ax, ylabel):
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.tick_params(axis="both", length=0)


def bar_panel(ax, data, metric, title, ylabel, fmt):
    """One bar per configuration (mean of repetitions), repetition values as dots, mean as a direct label."""
    for i, (prefix, name, color) in enumerate(CONFIGS):
        values = [metric(run) for run in data[prefix]]
        mean = statistics.fmean(values)
        ax.bar(i, mean, width=0.6, color=color, edgecolor=SURFACE, linewidth=2)
        ax.scatter([i] * len(values), values, s=22, color=SURFACE, edgecolor=TEXT_2, linewidth=1, zorder=3)
        # Above the bar and above every repetition dot, so the label never collides with a dot.
        ax.annotate(fmt.format(mean), (i, max(mean, *values)), xytext=(0, 8), textcoords="offset points",
                    ha="center", va="bottom", fontsize=11, fontweight="semibold")
    ax.set_xticks(range(len(CONFIGS)), [name for _, name, _ in CONFIGS])
    ax.set_title(title, loc="left", fontsize=12, fontweight="semibold", pad=10)
    style_axis(ax, ylabel)
    ax.margins(y=0.12)


def chart_fps(data, out):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.4), sharey=True)
    bar_panel(axes[0], data, lambda r: 1000.0 / statistics.fmean(r["frame"]), "Average FPS", "FPS", "{:.0f}")
    bar_panel(axes[1], data, lambda r: low_fps(r["frame"]), "1% low FPS (99th percentile frame time)", "", "{:.0f}")
    fig.suptitle("iRacing on AMD: 3 projections with SMP recover most of the single-projection frame rate",
                 x=0.01, ha="left", fontsize=13, fontweight="semibold")
    fig.text(0.01, 0.005, "Bars: mean of 3 repetitions · dots: each repetition · higher is better",
             color=TEXT_2, fontsize=9)
    fig.tight_layout(rect=(0, 0.03, 1, 0.97))
    fig.savefig(out, dpi=150)
    plt.close(fig)


def chart_busy(data, out):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.4), sharey=True)
    bar_panel(axes[0], data, lambda r: statistics.fmean(r["cpu"]), "CPU busy per frame", "ms per frame", "{:.2f} ms")
    bar_panel(axes[1], data, lambda r: statistics.fmean(r["gpu"]), "GPU busy per frame", "", "{:.2f} ms")
    fig.suptitle("SMP moves the cost of 3 projections from the CPU (the bottleneck) to the GPU",
                 x=0.01, ha="left", fontsize=13, fontweight="semibold")
    fig.text(0.01, 0.005, "PresentMon MsCPUBusy / MsGPUBusy · bars: mean of 3 repetitions · dots: each repetition · "
                          "lower is better", color=TEXT_2, fontsize=9)
    fig.tight_layout(rect=(0, 0.03, 1, 0.97))
    fig.savefig(out, dpi=150)
    plt.close(fig)


def chart_frametimes(data, out):
    """Frame time percentile curves, all repetitions pooled per configuration."""
    fig, ax = plt.subplots(figsize=(11, 4.6))
    # Direct labels: (x percentile, above/below the line). The two lower curves are close: one label above, one below.
    label_places = {"box_baseline": (70, -1), "box_3proj": (50, 1), "box_3proj_smp": (30, 1)}
    for prefix, name, color in CONFIGS:
        ordered = sorted(t for run in data[prefix] for t in run["frame"])
        n = len(ordered)
        x = [100.0 * i / (n - 1) for i in range(n)]
        ax.plot(x, ordered, color=color, linewidth=2, label=name)
        at, side = label_places[prefix]
        ax.annotate(name, (at, ordered[int((n - 1) * at / 100)]), xytext=(0, 7 * side), textcoords="offset points",
                    ha="center", va="bottom" if side > 0 else "top", fontsize=10, color=TEXT)
    # The slowest 0.1% (a handful of frames) is left out so that single spikes do not flatten the curves.
    ax.set_xlim(0, 99.9)
    ax.set_ylim(4, 14)
    ax.set_xlabel("Percentile of frames")
    style_axis(ax, "Frame time (ms)")
    ax.grid(axis="x", color=GRID, linewidth=0.8)
    ax.legend(frameon=False, loc="upper left")
    ax.set_title("Frame time distribution: SMP keeps 3 projections close to the single-projection curve",
                 loc="left", fontsize=13, fontweight="semibold", pad=10)
    fig.text(0.01, 0.005, "All repetitions pooled per configuration · slowest 0.1% of frames not shown · "
                          "lower is better", color=TEXT_2, fontsize=9)
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(out, dpi=150)
    plt.close(fig)


def main():
    runs_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    # The "<prefix>_r*" glob keeps "box_3proj" and "box_3proj_smp" apart.
    data = {prefix: load_runs(runs_dir, prefix) for prefix, _, _ in CONFIGS}
    assert all(len(runs) == 3 for runs in data.values()), {k: len(v) for k, v in data.items()}
    chart_fps(data, out_dir / "fps.png")
    chart_busy(data, out_dir / "cpu_gpu_busy.png")
    chart_frametimes(data, out_dir / "frame_times.png")
    print("Charts written to", out_dir)


if __name__ == "__main__":
    main()
