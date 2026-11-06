"""Logbook figures + raw-data excerpts, drawn straight from the experiment CSVs (read-only).

Run from the repo root:  python docs/figures/logbook_charts.py
Writes PNGs next to this file and prints markdown tables (summary + sampled raw rows) to stdout.
"""
import re
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent
plt.rcParams.update({"font.family": "Microsoft JhengHei", "font.size": 12, "axes.spines.top": False,
                     "axes.spines.right": False, "axes.unicode_minus": False})
NEST, ROOM, VENT, FAN = "#DC2626", "#2563EB", "#F59E0B", "#CBD5E1"
INK, MUTED = "#1E293B", "#64748B"
CH = [("巢穴", NEST), ("活動區", ROOM), ("出風口", VENT)]


def load(rel, valid_max=84.9):
    path = ROOT / rel
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    skip = next(i for i, l in enumerate(lines) if l.startswith("時間"))
    df = pd.read_csv(path, skiprows=skip, encoding="utf-8-sig")
    df = df[df["時間"].notna() & df["時間"].astype(str).str.contains(":")].copy()

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
    df = df.sort_values("t", kind="stable").reset_index(drop=True)
    df["min"] = (df["t"] - df["t"].iloc[0]).dt.total_seconds() / 60
    for c in ["巢穴", "活動區", "出風口", "風扇(%)"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")
    df.attrs["name"] = path.name
    df.attrs["raw"] = df[["巢穴", "活動區", "出風口"]].copy()
    # 85.00 = DS18B20 power-on value, >85 / spikes = bad contact: hide from plots, keep in raw stats
    for c, _ in CH:
        df.loc[df[c] > valid_max, c] = float("nan")
    return df


def gapped(df, col):
    """Break the line at log holes > 60 s instead of drawing a fake straight segment."""
    x = df["min"].to_numpy(dtype=float, copy=True)
    y = df[col].to_numpy(dtype=float, copy=True)
    gap = (df["t"].diff().dt.total_seconds() > 60).to_numpy()
    x[gap], y[gap] = float("nan"), float("nan")
    return x, y


def timeseries(ax, df, fan=True, chans=CH):
    if fan:
        ax2 = ax.twinx()
        ax2.fill_between(df["min"], df["風扇(%)"], step="post", color=FAN, alpha=0.5, lw=0)
        ax2.set_ylim(0, 300)
        ax2.set_yticks([0, 50, 100])
        ax2.set_yticklabels(["0%", "50%", "100%"], color=MUTED, fontsize=10)
        ax2.set_ylabel("冷端風扇（灰底）", color=MUTED, fontsize=10, loc="bottom")
        ax2.spines["right"].set_visible(True)
        ax2.spines["right"].set_color(FAN)
        ax.set_zorder(ax2.get_zorder() + 1)
        ax.patch.set_visible(False)
    for col, c in chans:
        if df[col].notna().any():
            ax.plot(*gapped(df, col), color=c, lw=2, label=col)
    ax.set_xlabel("時間（分鐘，從文件第一行算起）")
    ax.set_ylabel("溫度（°C）")
    ax.grid(color="#E2E8F0", lw=0.8)
    ax.set_xlim(0, df["min"].max())


def finish(fig, name, title, note):
    fig.text(0.01, 0.005, note, fontsize=9.5, color=MUTED)
    fig.tight_layout(rect=(0, 0.035, 1, 1))
    fig.savefig(OUT / name, dpi=150)
    plt.close(fig)


# ── markdown helpers ──
def md_summary(rows):
    print("| 數據 | 行數 | 時長 | 採樣間隔 | 巢穴 起→止（最低） | 活動區 起→止（最低） | 出風口 起→止（最低） |")
    print("|---|---|---|---|---|---|---|")
    for label, df in rows:
        dur = (df["t"].iloc[-1] - df["t"].iloc[0]).total_seconds() / 60
        dt = df["t"].diff().dt.total_seconds().median()
        cells = []
        for c, _ in CH:
            s = df[c].dropna()
            cells.append("—" if s.empty else f"{s.iloc[0]:.2f} → {s.iloc[-1]:.2f}（{s.min():.2f}）")
        print(f"| {label} | {len(df)} | {dur:.1f} min | {dt:.0f} s | " + " | ".join(cells) + " |")
    print()


def md_sample(df, every_min, title, extra_times=()):
    """Every N minutes pick the first row at or after that mark; plus any explicitly requested timestamps."""
    marks = [df["t"].iloc[0] + pd.Timedelta(minutes=m)
             for m in range(0, int(df["min"].max()) + 1, every_min)]
    marks += [pd.Timestamp(x) for x in extra_times]
    idx = sorted({df.index[df["t"] >= m][0] for m in marks if (df["t"] >= m).any()} | {df.index[-1]})
    print(f"**{title}**（每 {every_min} 分鐘取一行，原樣照抄）\n")
    print("| 時間 | 分鐘 | 巢穴 | 活動區 | 出風口 | 風扇 % | 狀態 |")
    print("|---|---|---|---|---|---|---|")
    raw = df.attrs["raw"]
    for i in idx:
        r = df.loc[i]
        vals = ["—" if pd.isna(raw.loc[i, c]) else f"{raw.loc[i, c]:.2f}" for c, _ in CH]
        fan = "—" if pd.isna(r["風扇(%)"]) else f"{r['風扇(%)']:.0f}"
        print(f"| {r['t']:%H:%M:%S} | {r['min']:.1f} | " + " | ".join(vals) + f" | {fan} | {r['狀態']} |")
    print()


P1 = "P1-无铜片/P1-无铜片_"
P3 = "P3-有铜片有隔温板/P3-有铜片有隔温板_"

# ── 07-22 TEC selection ──
sel = {k: load(f"{P1}0722_12703vs12706对比_{k}.csv") for k in ["12706制冷", "12703制冷", "12706加热", "12703加热"]}
fig, axs = plt.subplots(1, 2, figsize=(12, 4.6))
for ax, mode in zip(axs, ["制冷", "加热"]):
    for k, ls in [("12703", "-"), ("12706", "--")]:
        df = sel[k + mode]
        ax.plot(df["min"], df["巢穴"], color=NEST, ls=ls, lw=2, label=f"{k} 巢穴")
        ax.plot(df["min"], df["出風口"], color=VENT, ls=ls, lw=2, label=f"{k} 出風口")
    ax.set_title({"制冷": "(a) 製冷", "加热": "(b) 加熱"}[mode], loc="left", fontweight="bold")
    ax.set_xlabel("時間（分鐘）")
    ax.set_ylabel("溫度（°C）")
    ax.grid(color="#E2E8F0", lw=0.8)
    ax.legend(frameon=False, fontsize=10, ncol=2)
finish(fig, "fig_0722_tec選型.png", "07-22 TEC 選型：12703（實線）vs 12706（虛線）",
       "P1 無銅片，同一散熱器，12 V 滿壓，舊固件（比例控溫）。原始數據：P1-无铜片/0722_12703vs12706对比_*.csv")

# ── 07-23 8.5 V vs 12 V (cooling) ──
v85 = load(f"{P1}0723_12703降压8.5V制冷.csv")
v85h = load(f"{P1}0723_12703降压8.5V制热.csv")
fig, axs = plt.subplots(1, 2, figsize=(12, 4.6))
for ax, (a, b, t) in zip(axs, [(sel["12703制冷"], v85, "(a) 製冷"), (sel["12703加热"], v85h, "(b) 加熱")]):
    for df, ls, lab in [(a, "-", "12 V"), (b, "--", "8.5 V")]:
        ax.plot(df["min"], df["巢穴"], color=NEST, ls=ls, lw=2, label=f"{lab} 巢穴")
        ax.plot(df["min"], df["出風口"], color=VENT, ls=ls, lw=2, label=f"{lab} 出風口")
    ax.set_title(t, loc="left", fontweight="bold")
    ax.set_xlabel("時間（分鐘）")
    ax.set_ylabel("溫度（°C）")
    ax.grid(color="#E2E8F0", lw=0.8)
    ax.legend(frameon=False, fontsize=10, ncol=2)
finish(fig, "fig_0723_降壓.png", "07-23 降壓試驗：12703 在 12 V（實線）vs 8.5 V（虛線）",
       "12 V 為 07-22 數據（舊固件），8.5 V 為 07-23 17:02 後數據（開關式固件），固件不同只作粗比。")

# ── 07-24 63 min long run ──
p1 = load(f"{P1}0724_63分钟正常测试.csv")
fig, ax = plt.subplots(figsize=(12, 4.8))
timeseries(ax, p1)
ax.legend(loc="upper right", frameon=False, ncol=3)
finish(fig, "fig_0724_長測.png", "07-24 無銅片一小時製冷長測（P1 上限）",
       "18:31–19:34，開關式固件，中間有約 14 分鐘數據空洞（斷線處不連線）。原始數據：P1-无铜片/0724_63分钟正常测试.csv")

# ── 08-01 P2 fan slow-down ──
p2 = load("P2-有铜片无隔温板/P2-有铜片无隔温板_0801_风扇降速出风14.9度.csv")
fig, ax = plt.subplots(figsize=(12, 5))
timeseries(ax, p2)
ax.axhline(15, color=ROOM, ls=":", lw=1.2)
lo = p2["出風口"].idxmin()
ax.annotate(f"最低 {p2.loc[lo, '出風口']:.2f} °C（{p2.loc[lo, 't']:%H:%M:%S}）",
            (p2.loc[lo, "min"], p2.loc[lo, "出風口"]), xytext=(-200, 40), textcoords="offset points",
            color=VENT, fontweight="bold", arrowprops=dict(arrowstyle="-", color=VENT))
for t, txt in [("11:42", "100%"), ("11:54:20", "手動 50%"), ("11:59", "回升/停機"), ("12:06:05", "手動 52%")]:
    m = (pd.Timestamp(f"2026-08-01 {t}") - p2["t"].iloc[0]).total_seconds() / 60
    ax.axvline(m, color=MUTED, lw=0.8, ls="--")
    ax.text(m + 0.3, 27.3, txt, color=MUTED, fontsize=10)
ax.set_ylim(13, 28)
ax.legend(loc="lower left", frameon=False, ncol=3)
finish(fig, "fig_0801_風扇降速.png", "08-01 加銅片 + 密封箱：冷端風扇降速後出風口到 14.87 °C",
       "P2，室溫約 24–25 °C。虛線為手動操作時間點。原始數據：P2-有铜片无隔温板/0801_风扇降速出风14.9度.csv")

# ── 08-26 P3 XPS ──
p3 = load(f"{P3}0826_56分钟制冷出风16.9度.csv")
raw3 = p3.attrs["raw"]
fig, ax = plt.subplots(figsize=(12, 5))
timeseries(ax, p3)
bad = raw3[((raw3 - raw3.rolling(31, center=True, min_periods=1).median()).abs() > 2).any(axis=1)]
for i in bad.index:
    ax.axvline(p3.loc[i, "min"], color="#9333EA", lw=0.6, alpha=0.6)
cs = (pd.Timestamp("2026-08-26 16:35:13") - p3["t"].iloc[0]).total_seconds() / 60
ce = (pd.Timestamp("2026-08-26 17:13:48") - p3["t"].iloc[0]).total_seconds() / 60
ax.axvspan(cs, ce, color="#E0F2FE", alpha=0.5, zorder=0)
ax.text(cs + 0.5, 15.5, "連續製冷段 16:35–17:13", color=ROOM, fontsize=10)
ax.set_ylim(15, 32)
ax.legend(loc="upper right", frameon=False, ncol=3)
finish(fig, "fig_0826_XPS.png", "08-26 加 XPS 後：巢穴幾乎不降（條件太亂，不能歸因）",
       f"風扇 100%、室溫約 27–28 °C。紫色細線＝探頭跳變讀數（偏離前後中位數 >2 °C，共 {len(bad)} 行），85 °C 以上的值圖中不畫。")

# ── 08-31 aluminium plate ──
p31 = load(f"{P3}0831_43分钟巢穴探头缺失.csv")
fig, ax = plt.subplots(figsize=(12, 4.8))
timeseries(ax, p31)
m = p31.loc[(p31["風扇(%)"] == 62).idxmax(), "min"]
ax.axvline(m, color=MUTED, lw=0.8, ls="--")
ax.text(m + 0.3, ax.get_ylim()[1] - 0.6, "風扇 100% → 62%", color=MUTED, fontsize=10)
ax.legend(loc="center right", frameon=False, ncol=3)
finish(fig, "fig_0831_鋁板.png", "08-31 冷端夾鋁板：巢穴探頭掉線，只有活動區和出風口",
       "手動模式（保護不生效）。原始數據：P3-有铜片有隔温板/0831_43分钟巢穴探头缺失.csv（23 分鐘那份是同一次的前半段）")

# ── stage comparison ──
p2_slow = p2[p2["t"] >= pd.Timestamp("2026-08-01 12:06:05")]
p3c = p3[(p3["t"] >= pd.Timestamp("2026-08-26 16:35:13")) & (p3["t"] <= pd.Timestamp("2026-08-26 17:13:48"))]
stages = [("P1 無銅片\n07-24", p1), ("P2 銅片 52%\n08-01", p2_slow), ("P3 +XPS 100%\n08-26", p3c),
          ("P3 +鋁板\n08-31", p31)]
fig, ax = plt.subplots(figsize=(10, 4.6))
w = 0.26
for j, (col, c) in enumerate(CH):
    vals = [df[col].min() for _, df in stages]
    xs = [i + (j - 1) * w for i in range(len(stages))]
    bars = ax.bar(xs, [0 if pd.isna(v) else v for v in vals], w, color=c, label=f"{col}最低", zorder=3)
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + w / 2, (0 if pd.isna(v) else v) + 0.3, "缺" if pd.isna(v) else f"{v:.1f}",
                ha="center", fontsize=9.5, color=INK)
ax.axhline(15, color=ROOM, ls=":", lw=1.2, label="目標 15 °C")
ax.set_xticks(range(len(stages)), [s for s, _ in stages])
ax.set_ylabel("最低溫度（°C）")
ax.set_ylim(0, 30)
ax.grid(axis="y", color="#E2E8F0", zorder=0)
ax.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 1.1))
finish(fig, "fig_各階段最低溫.png", "各階段三個探頭的最低溫度",
       "P2 只取 52% 段，P3 08-26 只取連續製冷段。各次室溫、風速不同，只能看趨勢，不是對照實驗。")

# ── markdown output ──
md_summary([("07-22 12706 製冷", sel["12706制冷"]), ("07-22 12703 製冷", sel["12703制冷"]),
            ("07-22 12706 加熱", sel["12706加热"]), ("07-22 12703 加熱", sel["12703加热"]),
            ("07-23 12703 8.5V 製冷", v85), ("07-23 12703 8.5V 加熱", v85h),
            ("07-24 P1 63 分鐘", p1), ("08-01 P2 47 分鐘", p2), ("08-26 P3 XPS 連續製冷段", p3c),
            ("08-31 P3 鋁板", p31)])
md_sample(sel["12703制冷"], 2, "07-22 12703 製冷")
md_sample(sel["12706制冷"], 2, "07-22 12706 製冷")
md_sample(p1, 5, "07-24 P1 63 分鐘長測")
md_sample(p2, 4, "08-01 P2", extra_times=["2026-08-01 11:54:20", "2026-08-01 12:06:05", "2026-08-01 12:28:03"])
md_sample(p3, 5, "08-26 P3 XPS")
md_sample(p31, 5, "08-31 P3 鋁板")
print("**08-26 探頭跳變讀數**（連續相同的只列第一行）\n")
print("| 時間 | 巢穴 | 活動區 | 出風口 | 風扇 % | 狀態 |")
print("|---|---|---|---|---|---|")
prev = None
for i in bad.index:
    key = tuple(raw3.loc[i])
    if key != prev:
        r = p3.loc[i]
        print(f"| {r['t']:%H:%M:%S} | " + " | ".join(f"{v:.2f}" for v in key) + f" | {r['風扇(%)']:.0f} | {r['狀態']} |")
    prev = key
