#!/usr/bin/env python3
"""Render docs/img/scan_timing.png from the timing evidence JSON files (needs matplotlib)."""
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
EV = ROOT / 'docs' / 'evidence'
SOURCES = [  # (label prefix, file)
    ('Windows · sleep_until (before)', 'timing_windows_before_adr011_2026-10-19.json'),
    ('Windows · high-res timer', 'timing_windows_2026-10-19.json'),
    ('Linux/WSL · clock_nanosleep', 'timing_linux_wsl_2026-10-19.json'),
]
SURFACE, INK, MUTED, GRID = '#fcfcfb', '#1f1e1c', '#6b6a64', '#e6e5e0'
P50, P99 = '#2a78d6', '#eb6834'  # categorical slots 1-2, validated (CVD ΔE 24.7)

rows = []
for prefix, name in SOURCES:
    for r in json.loads((EV / name).read_text(encoding='utf-8'))['results']:
        cond = ('CPU stress' if r['cpu_stress_processes'] else 'idle') + (' + RT' if r['rt_tuning_requested'] else '')
        rows.append((f'{prefix}\n{cond}', r['lateness_p50_us'], r['lateness_p99_us']))
rows.reverse()  # first source on top

fig, ax = plt.subplots(figsize=(9.5, 6.4), dpi=150)
fig.patch.set_facecolor(SURFACE)
ax.set_facecolor(SURFACE)
y = range(len(rows))
h = 0.36
ax.barh([i + h / 2 + 0.02 for i in y], [r[2] for r in rows], height=h, color=P99, label='p99')
ax.barh([i - h / 2 - 0.02 for i in y], [r[1] for r in rows], height=h, color=P50, label='p50')
for i, r in enumerate(rows):  # direct labels on p99 only (selective)
    ax.text(r[2] * 1.12, i + h / 2 + 0.02, f'{r[2]:,} µs'.replace(',', ' '), va='center', fontsize=8, color=INK)
ax.set_xscale('log')
ax.set_xlim(40, 400000)
ax.axvline(50000, color=MUTED, lw=1, ls=(0, (4, 3)))
ax.set_ylim(-1.3, len(rows) - 0.4)
ax.text(50000 * 1.07, -0.95, 'one full period (50 ms)', fontsize=8, color=MUTED, va='center')
ax.set_yticks(list(y))
ax.set_yticklabels([r[0] for r in rows], fontsize=8, color=INK)
ax.set_xlabel('scan wake-up lateness (µs, log scale) — lower is better', fontsize=9, color=MUTED)
ax.tick_params(axis='x', colors=MUTED, labelsize=8)
ax.grid(axis='x', color=GRID, lw=0.8)
ax.set_axisbelow(True)
for side in ('top', 'right', 'left'):
    ax.spines[side].set_visible(False)
ax.spines['bottom'].set_color(GRID)
ax.legend(loc='lower right', bbox_to_anchor=(1.0, 1.0), ncol=2, frameon=False, fontsize=9, labelcolor=INK)
ax.set_title('Soft-PLC scan timing — 50 ms period, 15 s windows (measured 2026-10-19)',
             fontsize=11, color=INK, loc='left', pad=12)
fig.tight_layout()
out = ROOT / 'docs' / 'img' / 'scan_timing.png'
out.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(out, facecolor=SURFACE)
print('Saved:', out)
