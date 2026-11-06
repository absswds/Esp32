"""Poster charts drawn straight from the raw experiment CSVs (read-only).

Run from the repo root:  python poster/charts.py
"""
import re
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(__file__).resolve().parent
plt.rcParams.update({"font.family": "Microsoft JhengHei", "font.size": 15, "axes.spines.top": False,
                     "axes.spines.right": False, "axes.unicode_minus": False})
NEST, ROOM, VENT = "#DC2626", "#2563EB", "#F59E0B"
INK, MUTED = "#1E293B", "#64748B"


def load(rel):
    path = ROOT / rel
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    skip = next(i for i, l in enumerate(lines) if l.startswith("時間"))
    df = pd.read_csv(path, skiprows=skip, encoding="utf-8-sig")
    df = df[df["時間"].notna() & df["時間"].str.contains(":")]

    def parse(s):
        m = re.match(r"(\d+)/(\d+)/(\d+) (上午|下午)?(\d+):(\d+)(?::(\d+))?", s)
        y, mo, d, ap, h, mi, sec = m.groups()
        h = int(h)
        if ap == "下午" and h < 12:
            h += 12
        if ap == "上午" and h == 12:
            h = 0
        return pd.Timestamp(int(y), int(mo), int(d), h, int(mi), int(sec or 0))

    df["t"] = df["時間"].map(parse)
    df["min"] = (df["t"] - df["t"].iloc[0]).dt.total_seconds() / 60
    for c in ["巢穴", "活動區", "出風口", "風扇(%)"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")
    return df


def rate(df):
    sec = (df["t"].iloc[-1] - df["t"].iloc[0]).total_seconds()
    return abs(df["巢穴"].iloc[-1] - df["巢穴"].iloc[0]) / sec * 60


# ── numbers ──
p1 = load("P1-无铜片/P1-无铜片_0724_63分钟正常测试.csv")
p2 = load("P2-有铜片无隔温板/P2-有铜片无隔温板_0801_风扇降速出风14.9度.csv")
p2_full = p2[p2["t"] < pd.Timestamp("2026-08-01 11:54:20")]
p2_slow = p2[p2["t"] >= pd.Timestamp("2026-08-01 12:06:05")]
stage = {
    "P1 無銅片": p1["出風口"].min(),
    "P2 加銅片\n風扇 100%": p2_full["出風口"].min(),
    "P2 冷端風扇\n降至 52%": p2_slow["出風口"].min(),
}
low_row = p2.loc[p2["出風口"].idxmin()]
tec = {k: rate(load(f"P1-无铜片/P1-无铜片_0722_12703vs12706对比_{k}.csv"))
       for k in ["12703制冷", "12706制冷", "12703加热", "12706加热"]}
for k, v in {**stage, **tec}.items():
    print(f"{k.replace(chr(10), ' ')}: {v:.3f}")
print("P2 lowest outlet row:", low_row["時間"], low_row["巢穴"], low_row["活動區"], low_row["出風口"])

# ── Fig 1: outlet minimum across iterations ──
fig, ax = plt.subplots(figsize=(8, 5.2), dpi=200)
names, vals = list(stage), list(stage.values())
cols = ["#94A3B8", "#F59E0B", "#2563EB"]
bars = ax.bar(names, vals, color=cols, width=0.56, zorder=3)
for b, v in zip(bars, vals):
    ax.text(b.get_x() + b.get_width() / 2, v + 0.4, f"{v:.2f} °C", ha="center", fontsize=17, fontweight="bold", color=INK)
for i in range(2):
    d = vals[i + 1] - vals[i]
    ax.annotate("", xy=(i + 0.75, vals[i + 1] + 2.2), xytext=(i + 0.25, vals[i] + 2.2),
                arrowprops=dict(arrowstyle="-|>", color="#16A34A", lw=2.5))
    ax.text(i + 0.55, (vals[i] + vals[i + 1]) / 2 + 2.9, f"{d:+.1f} °C", ha="left", color="#16A34A",
            fontsize=16, fontweight="bold")
ax.axhline(15, color=ROOM, ls="--", lw=1.5, zorder=2)
ax.text(-0.42, 15.4, "論文目標 15 °C", color=ROOM, fontsize=12, ha="left", zorder=4,
        bbox=dict(facecolor="white", edgecolor="none", pad=1))
ax.set_ylim(0, 30)
ax.set_ylabel("出風口最低溫度（°C）")
ax.grid(axis="y", color="#E2E8F0", zorder=0)
ax.set_title("每一階段改進，出風口溫度都再降一截", fontsize=18, fontweight="bold", color=INK, loc="left")
fig.text(0.01, 0.01, "數據：P1 0724 63 分鐘長測；P2 0801 47 分鐘實測。出風口達 14.9 °C 時巢穴仍約 22 °C。",
         fontsize=10.5, color=MUTED)
fig.tight_layout(rect=(0, 0.04, 1, 1))
fig.savefig(OUT / "圖_迭代改善.png")

# ── Fig 2: P2 time series with fan duty shading ──
fig, ax = plt.subplots(figsize=(11, 5.2), dpi=200)
ax2 = ax.twinx()
ax2.fill_between(p2["min"], p2["風扇(%)"], step="post", color="#CBD5E1", alpha=0.45, lw=0)
ax2.set_ylim(0, 300)
ax2.set_yticks([0, 52, 100])
ax2.set_yticklabels(["0%", "52%", "100%"], color=MUTED, fontsize=12)
ax2.spines["right"].set_visible(True)
ax2.spines["right"].set_color("#CBD5E1")
ax2.set_ylabel("冷端風扇", color=MUTED, fontsize=12, loc="bottom")
ax.set_zorder(ax2.get_zorder() + 1)
ax.patch.set_visible(False)
for col, c, lab in [("巢穴", NEST, "巢穴"), ("活動區", ROOM, "活動區"), ("出風口", VENT, "出風口")]:
    ax.plot(p2["min"], p2[col], color=c, lw=2.6, label=lab)
ax.axhline(15, color=ROOM, ls="--", lw=1.3)
lo = p2["出風口"].idxmin()
ax.scatter(p2.loc[lo, "min"], p2.loc[lo, "出風口"], s=90, color=VENT, edgecolor="white", lw=2, zorder=5)
ax.annotate(f"降速後最低 {p2.loc[lo, '出風口']:.2f} °C", (p2.loc[lo, "min"], p2.loc[lo, "出風口"]),
            xytext=(-240, 34), textcoords="offset points", fontsize=14, fontweight="bold", color=VENT,
            arrowprops=dict(arrowstyle="-", color=VENT))
full_min = p2_full["出風口"].min()
ax.annotate(f"風扇 100%：停在 {full_min:.1f} °C", (p2_full["min"].iloc[-1], full_min),
            xytext=(-30, -42), textcoords="offset points", fontsize=13, color=INK, ha="right",
            arrowprops=dict(arrowstyle="-", color=MUTED))
ax.set_ylim(12, 28)
ax.set_xlim(0, p2["min"].max())
ax.set_xlabel("時間（分鐘）")
ax.set_ylabel("溫度（°C）")
ax.grid(color="#E2E8F0")
ax.legend(loc="upper right", ncol=3, frameon=False, fontsize=13)
ax.set_title("反直覺發現：冷端風扇降速，出風口反而更冷", fontsize=18, fontweight="bold", color=INK, loc="left")
fig.text(0.01, 0.01, "P2（08-01，有銅片、未裝 XPS）。灰色底＝風扇占空比；50%/52% 為網頁手動設定，中段曾停機重啟。",
         fontsize=10.5, color=MUTED)
fig.tight_layout(rect=(0, 0.04, 1, 1))
fig.savefig(OUT / "圖_風扇降速.png")

# ── Fig 3: TEC selection ──
fig, ax = plt.subplots(figsize=(8, 5.2), dpi=200)
x = [0, 1]
w = 0.36
r03 = [tec["12703制冷"], tec["12703加热"]]
r06 = [tec["12706制冷"], tec["12706加热"]]
b1 = ax.bar([i - w / 2 for i in x], r03, w, color="#16A34A", label="TEC1-12703（約 36 W）", zorder=3)
b2 = ax.bar([i + w / 2 for i in x], r06, w, color="#94A3B8", label="TEC1-12706（約 72 W）", zorder=3)
for bs in (b1, b2):
    for b in bs:
        ax.text(b.get_x() + b.get_width() / 2, b.get_height() + 0.03, f"{b.get_height():.2f}", ha="center",
                fontsize=14, fontweight="bold", color=INK)
for i in x:
    pct = (r03[i] / r06[i] - 1) * 100
    ax.text(i, max(r03[i], r06[i]) + 0.2, f"12703 快 {pct:.0f}%", ha="center", color="#16A34A",
            fontsize=14, fontweight="bold")
ax.set_xticks(x, ["製冷速率", "加熱速率"])
ax.set_ylabel("巢穴溫度變化速率（°C/min）")
ax.set_ylim(0, 1.9)
ax.grid(axis="y", color="#E2E8F0", zorder=0)
ax.legend(frameon=False, fontsize=13, loc="upper left")
ax.set_title("TEC 選型：一半功耗，製冷加熱都更快", fontsize=18, fontweight="bold", color=INK, loc="left")
fig.text(0.01, 0.01, "P1 07-22，同一散熱器、12 V 滿壓；功耗為 12 V × 額定電流估算值。",
         fontsize=10.5, color=MUTED)
fig.tight_layout(rect=(0, 0.04, 1, 1))
fig.savefig(OUT / "圖_TEC選型.png")


# ── Fig 4: nest temperature across the three build stages ──
def gapped(df, col):
    """Break the line where the log has a hole longer than 60 s instead of drawing a fake straight segment."""
    x, y = df["min"].to_numpy(dtype=float, copy=True), df[col].to_numpy(dtype=float, copy=True)
    gap = (df["t"].diff().dt.total_seconds() > 60).to_numpy()
    x[gap], y[gap] = float("nan"), float("nan")
    return x, y


# P3 (08-26): only the continuous-cooling window; probe glitches (85 °C reset value, EMA artefacts) all fall before it
p3 = load("P3-有铜片有隔温板/P3-有铜片有隔温板_0826_56分钟制冷出风16.9度.csv")
p3 = p3[(p3["t"] >= pd.Timestamp("2026-08-26 16:35:13")) & (p3["t"] <= pd.Timestamp("2026-08-26 17:13:48"))].copy()
p3["min"] = (p3["t"] - p3["t"].iloc[0]).dt.total_seconds() / 60

plt.rcParams.update({"font.size": 16})
fig, (ax, bx) = plt.subplots(1, 2, figsize=(16, 6), dpi=220, gridspec_kw={"wspace": 0.18})
runs = [(p1, "#64748B", "P1 無銅片"),
        (p2, VENT, "P2 加銅片 + 風扇降速"),
        (p3, "#7C3AED", "P3 再加 XPS 保溫")]


def panel(a, col, title):
    """Cooling relative to each run's own start (start - T), so different starting temperatures don't mislead."""
    for k, (df, c, lab) in enumerate(runs):
        s_ = df[col].dropna()
        drop = df[col].iloc[0] if pd.notna(df[col].iloc[0]) else s_.iloc[0]
        d = df.assign(**{col: drop - df[col]})
        a.plot(*gapped(d, col), color=c, lw=2.8, label=lab)
        i = s_.idxmin()
        best, t_best = drop - s_[i], df.loc[i, "min"]
        rate = best / t_best if t_best > 0 else 0.0
        summary = f"降 {best:.1f} °C｜平均 {rate:.2f} °C/min" if best > 0.05 else "未見下降"
        if best > 0.05:
            a.scatter(t_best, best, s=60, color=c, edgecolor="white", lw=1.6, zorder=5)
        a.text(0.02, 0.97 - 0.085 * k, f"{lab.split(' ')[0]}　{summary}",
               transform=a.transAxes, color=c, fontsize=15, fontweight="bold", va="top")
    a.axhline(0, color="#94A3B8", lw=1)
    a.set_xlim(0, 70)
    a.set_xlabel("製冷時間（min）")
    a.set_ylabel("相對起點降溫（°C）")
    a.grid(color="#E2E8F0", lw=0.8)
    a.set_title(title, loc="left", fontsize=19, fontweight="bold", color=INK)


panel(ax, "巢穴", "(a) 巢穴降溫量")
panel(bx, "出風口", "(b) 出風口降溫量")
ax.set_ylim(-4.5, 8.5)
bx.set_ylim(-6, 17)
handles, labels = ax.get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=3, frameon=False, fontsize=16, bbox_to_anchor=(0.5, 1.07))
fig.savefig(OUT / "圖_實驗結果.png", bbox_inches="tight")
print("saved")
