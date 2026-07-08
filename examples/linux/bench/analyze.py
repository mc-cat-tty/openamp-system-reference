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
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


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

    # histogram, normalized to each series' median -> compares jitter shape
    plt.figure(figsize=(9, 5))
    for lbl, x, _ in data:
        plt.hist(x / np.median(x), bins=200, histtype="step",
                 density=True, log=True, label=lbl)
    plt.xlabel("execution time / median")
    plt.ylabel("density (log)")
    plt.title("Execution-time distribution (normalized to median)")
    plt.legend()
    plt.tight_layout()
    plt.savefig(f"{a.out}_hist.png", dpi=130)

    # CCDF: P(time > x), log-y -> the tail is the story
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

    print(f"\nsaved {a.out}_hist.png and {a.out}_ccdf.png")


if __name__ == "__main__":
    main()
