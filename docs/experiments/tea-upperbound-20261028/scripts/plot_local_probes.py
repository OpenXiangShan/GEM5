#!/usr/bin/env python3
"""Render historical cold-start probes with corrected window labels."""

import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def main():
    root = Path(__file__).resolve().parents[1]
    cases = []
    for name in ['gcc_expr_1687', 'mcf_12886']:
        data = json.loads((root / 'data' / f'tea-{name}.json').read_text())
        for label, side in zip(['base', '16-wide'], data['sides']):
            cases.append((name.split('_')[0] + ' ' + label, side['state_cpi']))
    fig, ax = plt.subplots(figsize=(8.5, 4.8), constrained_layout=True)
    bottom = np.zeros(len(cases))
    for state, color in zip(['Computing', 'Stalled', 'Flushed', 'Drained'],
                            ['#498ab5', '#ce7668', '#d9b158', '#98a4b3']):
        values = [side[state] for _, side in cases]
        ax.bar(range(len(cases)), values, bottom=bottom, label=state,
               color=color, width=0.62)
        bottom += values
    ax.set_xticks(range(len(cases)), [label for label, _ in cases])
    ax.set_ylabel('CPI; mutually exclusive states')
    ax.set_title('Legacy 16-wide probe: first 2M instructions after restore')
    ax.legend(ncol=4, loc='upper left')
    ax.set_ylim(0, max(bottom) * 1.2)
    fig.savefig(root / 'figures' / 'tea-residual-states.png', dpi=180)
    plt.close(fig)

    rows = json.loads((root / 'data' / 'formal-frontend-ablations.json')
                      .read_text())
    variants = [('baseline', 'Baseline'), ('predict1', 'Second prediction off'),
                ('fetch1', 'Second fetch off'),
                ('override_bubbles', 'Override bubbles on'),
                ('timing_fetch', 'Timing instruction fetch')]
    fig, ax = plt.subplots(figsize=(9.5, 4.8), constrained_layout=True)
    for i, workload in enumerate(['perlbench_checkspam_5886',
                                  'libquantum_69175']):
        values = {row['variant']: row['ipc'] for row in rows
                  if row['workload'] == workload}
        deltas = [(values[key] / values['frontend'] - 1) * 100
                  for key, _ in variants]
        ax.barh(np.arange(len(variants)) + (i - 0.5) * 0.35, deltas,
                height=0.32, label=workload, color=['#4186b1', '#dc9b54'][i])
    ax.set_yticks(range(len(variants)), [label for _, label in variants])
    ax.invert_yaxis()
    ax.axvline(0, color='#8390a0', linewidth=0.8)
    ax.set_xlabel('IPC change vs full ideal frontend (%)')
    ax.set_title('Cold-start ablations: first 2M instructions after restore')
    ax.legend(loc='lower left')
    fig.savefig(root / 'figures' / 'formal-frontend-ablations.png', dpi=180)
    plt.close(fig)


if __name__ == '__main__':
    main()
