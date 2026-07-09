#!/usr/bin/env python3
"""
Analyze bench samples: A53 (ns) vs R5 (cycles).
  python3 analyze.py a53_samples.csv r5_samples.csv --labels A53 R5 --r5-mhz 500

CSV format: a "# unit=<ns|cycles> count=N" header line, then one integer per line.
R5 'cycles' are converted to ns via --r5-mhz (set it to your RPU clock).

R5 clock frequency can be found in BSP's xparameters.h XPAR_CPU_CORE_CLOCK_FREQ_HZ; in my case is 533328002
"""
import argparse
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import seaborn as sns


def load(path, r5_hz):
    unit, vals = None, []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                for tok in line[1:].split():
                    if tok.startswith("unit="):
                        unit = tok.split("=", 1)[1]
                continue
            vals.append(int(line))
    x = np.asarray(vals, dtype=np.float64)
    if unit == "cycles":            # -> nanoseconds
        x = x / r5_hz * 1e9
        unit = "ns(from cycles)"
    return x, unit


def stats(x):
    return dict(n=len(x), mn=x.min(), med=np.median(x),
                p99=np.percentile(x, 99), p999=np.percentile(x, 99.9),
                mx=x.max(), cv=x.std() / x.mean(), spread=x.max() / x.min())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--labels", nargs="*", default=None)
    ap.add_argument("--r5-hz", type=int, default=533328002,
                    help="RPU clock (Hz) for cycles->ns")
    ap.add_argument("--out", default="bench")
    a = ap.parse_args()
    r5_hz = a.r5_hz

    data = []
    for i, fp in enumerate(a.files):
        x, unit = load(fp, r5_hz)
        lbl = a.labels[i] if a.labels and i < len(a.labels) else fp
        data.append((lbl, x, unit))

    hdr = (f"{'label':<20}{'n':>7}{'min':>10}{'median':>10}"
           f"{'p99':>10}{'p99.9':>11}{'max':>11}{'CV%':>8}{'max/min':>9}")
    print(hdr)
    print("-" * len(hdr))
    for lbl, x, _ in data:
        s = stats(x)
        print(f"{lbl:<20}{s['n']:>7}{s['mn']:>10.1f}{s['med']:>10.1f}"
              f"{s['p99']:>10.1f}{s['p999']:>11.1f}{s['mx']:>11.1f}"
              f"{s['cv']*100:>7.2f}{s['spread']:>9.1f}")
    print("\ntimes in ns; CV = std/mean (jitter metric); max/min = worst-case spread")

    # histogram, raw execution time -> compares actual timing, not just shape
    # median (dashed) per series as decoration
    plt.figure(figsize=(9, 5))
    for lbl, x, _ in data:
        x_us = x / 1e3
        line = plt.hist(x_us, bins=200, histtype="step",
                         density=True, log=True, label=lbl)
        color = line[2][0].get_edgecolor()
        plt.axvline(np.median(x_us), color=color, linestyle="--",
                    linewidth=1, alpha=0.8)
    plt.xlabel("execution time (us)")
    plt.ylabel("density (log)")
    plt.title("Execution-time distribution (dashed = median)")
    plt.legend()
    plt.tight_layout()
    plt.savefig(f"{a.out}_hist.png", dpi=130)

    # CCDF: P(time > x), log-y -> the tail is the story
    # combined: all domains on one shared axis
    plt.figure(figsize=(9, 5))
    for lbl, x, _ in data:
        xs = np.sort(x)
        ccdf = 1.0 - np.arange(len(xs)) / len(xs)
        plt.semilogy(xs / np.median(x), ccdf, label=lbl)
    plt.xlabel("execution time / median")
    plt.ylabel("P(time > x)")
    plt.title("Tail distribution (CCDF)")
    plt.grid(True, which="both", alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(f"{a.out}_ccdf.png", dpi=130)

    # separate: each domain gets its own subplot/axis: R5's jitter is orders
    # of magnitude tighter than A53's, so a shared scale crushes one of them.
    # laid out on a square-ish grid rather than a single row, so it doesn't
    # get absurdly wide as more experiments are added
    n = len(data)
    ncols = int(np.ceil(np.sqrt(n)))
    nrows = int(np.ceil(n / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(6 * ncols, 5 * nrows),
                             squeeze=False)
    axes_flat = axes.flatten()
    for ax, (lbl, x, _) in zip(axes_flat, data):
        xs = np.sort(x)
        ccdf = 1.0 - np.arange(len(xs)) / len(xs)
        ax.semilogy(xs / np.median(x), ccdf, label=lbl)
        ax.set_xlabel("execution time / median")
        ax.set_ylabel("P(time > x)")
        ax.set_title(lbl)
        ax.grid(True, which="both", alpha=0.3)
        # disable offset notation: for narrow ranges (e.g. R5) matplotlib
        # relabels ticks relative to a "+1" corner offset, which reads as
        # if the axis starts at 0.0 instead of ~1.0
        ax.ticklabel_format(useOffset=False, style="plain", axis="x")
        ax.margins(x=0)
    for ax in axes_flat[n:]:
        ax.set_visible(False)
    fig.suptitle("Tail distribution (CCDF, separate axes)")
    plt.tight_layout()
    plt.savefig(f"{a.out}_ccdf_separate.png", dpi=130)

    # long-form frame + shared ordering for the seaborn plot below
    df = pd.concat(
        [pd.DataFrame({"time_us": x / 1e3, "domain": lbl}) for lbl, x, _ in data],
        ignore_index=True)
    order = [lbl for lbl, _, _ in data]

    # horizontal boxplot with raw observations overlaid, log-x execution time
    with sns.axes_style("ticks"):
        fig, ax = plt.subplots(figsize=(10, 1.2 * len(order) + 1))
        ax.set_xscale("log")
        sns.boxplot(df, x="time_us", y="domain", order=order, hue="domain",
                   whis=[0, 100], width=0.6, palette="vlag", legend=False, ax=ax)
        sns.stripplot(df, x="time_us", y="domain", order=order,
                     size=4, color=".3", ax=ax)
        ax.xaxis.grid(True)
        ax.set(xlabel="execution time (us, log scale)", ylabel="",
              title="Execution-time spread by experiment")
        sns.despine(trim=True, left=True)
        plt.tight_layout()
        fig.savefig(f"{a.out}_box.png", dpi=150)

    print(f"\nsaved {a.out}_hist.png, {a.out}_ccdf.png, {a.out}_ccdf_separate.png "
          f"and {a.out}_box.png")


if __name__ == "__main__":
    main()
