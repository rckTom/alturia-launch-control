"""Plot the CalculiX results: web_force.png (key force) and seal_plate.png
(retainer groove floor). Reads the job.dat files in work/ directly, so it also
works while web_force.py / seal.py are still running."""
import glob
import json
import os
import re

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
SERIES = {0.3: "#2a78d6", 0.4: "#eb6834", 0.5: "#1baf7a"}
INK, INK2, GRID, SURF = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
CLICK, BOTTOM = 0.35, 0.45     # preload 0.1 + 0.25 switch travel, +0.1 overtravel


WORK = os.path.join(HERE, "work")
STROKE = 0.6
E_REF, E_LO = 26.0, 10.0


def curves():
    out = []
    for d in sorted(glob.glob(os.path.join(WORK, "*_h2"))):
        m = re.match(r"(sq|rd)_t([\d.]+)_(clamped|sliding)_h2", os.path.basename(d))
        dat = os.path.join(d, "job.dat")
        if not m or not os.path.exists(dat):
            continue
        txt = open(dat).read()
        cv = [(0.0, 0.0)] + [(float(a.group(1)) * STROKE, -4 * float(a.group(4))) for a in re.finditer(
            r"set STEM and time\s+([-\dE.+]+)\s*\n\s*([-\dE.+]+)\s+([-\dE.+]+)\s+([-\dE.+]+)", txt)]
        if cv[-1][0] > STROKE - 1e-6:
            out.append({"shape": m.group(1), "t": float(m.group(2)), "clamp": m.group(3), "curve": cv})
    return out


def main():
    res = curves()
    e_ref, e_lo = E_REF, E_LO
    plt.rcParams.update({"font.size": 10, "axes.edgecolor": INK2, "axes.labelcolor": INK2,
                         "xtick.color": INK2, "ytick.color": INK2})
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.4), sharey=True, facecolor=SURF)
    for ax, (shape, title) in zip(axes, (("sq", "Outer keys (8 mm square)"),
                                         ("rd", "Enter (Ø9 round)"))):
        ax.set_facecolor(SURF)
        for x in (CLICK, BOTTOM):
            ax.axvline(x, color=INK2, lw=1, ls=(0, (2, 3)), zorder=1)
        ax.text(CLICK, 1.02, " click", color=INK2, fontsize=8, transform=ax.get_xaxis_transform(), ha="center")
        ax.text(BOTTOM, 1.02, " bottomed", color=INK2, fontsize=8, transform=ax.get_xaxis_transform(), ha="center")
        for t, col in SERIES.items():
            cur = {r["clamp"]: r["curve"] for r in res if r["shape"] == shape and r["t"] == t}
            if len(cur) < 2:
                continue
            hi, lo = cur["clamped"], cur["sliding"]
            xs = [p[0] for p in hi]
            ax.fill_between(xs, [p[1] for p in lo], [p[1] for p in hi], color=col, alpha=0.18, lw=0, zorder=2)
            ax.plot(xs, [p[1] for p in hi], color=col, lw=2, zorder=3)
            ax.plot([p[0] for p in lo], [p[1] for p in lo], color=col, lw=2, ls=(0, (4, 2)), zorder=3)
            ax.text(xs[-1] + 0.01, hi[-1][1], "%.1f mm" % t, color=INK, va="center", fontsize=9)
        ax.set_title(title, color=INK, fontsize=11, loc="left")
        ax.set_xlabel("stem travel from free membrane (mm)")
        ax.set_xlim(0, 0.68)
        ax.grid(color=GRID, lw=0.8)
        ax.spines[["top", "right"]].set_visible(False)
    axes[0].set_ylabel("force added by the web (N)")
    axes[0].set_ylim(0, None)
    h = [plt.Line2D([], [], color=INK2, lw=2), plt.Line2D([], [], color=INK2, lw=2, ls=(0, (4, 2)))]
    fig.legend(h, ["clamped edge", "frictionless sandwich"], loc="upper left", frameon=False,
               labelcolor=INK2, ncol=2, bbox_to_anchor=(0.0, 0.95))
    fig.suptitle("TPU 95A web, E = %.0f MPa (all forces x%.2f at E = %.0f MPa); switch alone is ~1.6 N"
                 % (e_ref, e_lo / e_ref, e_lo), x=0.01, ha="left", color=INK, fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    fig.savefig(os.path.join(HERE, "web_force.png"), dpi=150, facecolor=SURF)
    for r in res:
        cv = list(zip(*r["curve"]))
        print("%s t=%.1f %-8s F(0.35)=%.3f F(0.45)=%.3f N" % (r["shape"], r["t"], r["clamp"],
              np.interp(CLICK, *cv), np.interp(BOTTOM, *cv)))


def plate_map(d):
    """Groove-floor uz (positive = away from the frame) from a seal.py plate run."""
    dat, mesh = os.path.join(d, "job.dat"), os.path.join(d, "mesh.inp")
    if not os.path.exists(dat) or os.path.getsize(dat) == 0:
        return None
    blk = open(dat).read().split("displacements (vx,vy,vz) for set RIDGE")[1].split("forces")[0]
    uz = {int(p[0]): -float(p[3]) for p in (l.split() for l in blk.splitlines()) if len(p) == 4}
    xy = {}
    with open(mesh) as f:
        next(f)
        for line in f:
            if line.startswith("*"):
                break
            p = line.split(",")
            n = int(p[0])
            if n in uz:
                xy[n] = (float(p[1]) + 12.5, float(p[2]) + 2.0)
    return np.array([[*xy[n], u] for n, u in uz.items()])


def plates():
    variants = [("plate", "2.4 mm plate"), ("plate_t3", "3.0 mm plate"),
                ("plate_t34", "3.4 mm plate"), ("plate_ribs", "2.4 mm + 1.0 mm ribs")]
    maps = [(lbl, plate_map(os.path.join(WORK, n))) for n, lbl in variants]
    maps = [(l, m) for l, m in maps if m is not None]
    if not maps:
        return
    sq = 0.3
    vmax = max(m[:, 2].max() for _, m in maps)
    fig, axes = plt.subplots(1, len(maps), figsize=(3.6 * len(maps) + 1.2, 4.2), facecolor=SURF,
                             squeeze=False, layout="constrained")
    for ax, (lbl, m) in zip(axes[0], maps):
        ax.set_facecolor(SURF)
        sc = ax.scatter(m[:, 0], m[:, 1], c=m[:, 2], s=3, cmap="Blues", vmin=0, vmax=vmax)
        for sx in (-1, 1):
            for sy in (-1, 1):
                ax.plot(14.5 * sx, 14.5 * sy, marker="+", color=INK, ms=9, mew=1.5)
        relaxed = m[:, 2] * sq / (sq + np.maximum(m[:, 2], 0))
        ax.set_title("%s\nlift max %.3f mm\nsqueeze left >= %.2f of %.1f mm" % (lbl, m[:, 2].max(), sq - relaxed.max(), sq),
                     color=INK, fontsize=9.5, loc="left")
        ax.set_aspect("equal")
        ax.set_xticks([]), ax.set_yticks([])
        for sp in ax.spines.values():
            sp.set_visible(False)
        print("%-22s floor lift max %.3f mean %.3f mm unrelaxed; relaxed max %.3f -> squeeze left %.3f"
              % (lbl, m[:, 2].max(), m[:, 2].mean(), relaxed.max(), sq - relaxed.max()))
    cb = fig.colorbar(sc, ax=axes[0].tolist(), shrink=0.8, pad=0.03)
    cb.set_label("groove floor lift (mm, unrelaxed)", color=INK2)
    fig.suptitle("Retainer under the lip load (68 N), screws at +; lip squeeze 0.3 mm",
                 x=0.01, ha="left", color=INK, fontsize=11)
    fig.savefig(os.path.join(HERE, "seal_plate.png"), dpi=150, facecolor=SURF, bbox_inches="tight")


if __name__ == "__main__":
    main()
    plates()
