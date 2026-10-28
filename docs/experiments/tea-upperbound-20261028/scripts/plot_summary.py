#!/usr/bin/env python3
"""Render the full-profile score comparison from the experiment manifest."""

import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    root = Path(__file__).resolve().parents[1]
    runs = {run['label']: run for run in json.loads(
        (root / 'runs.json').read_text())}
    groups = [('baseline24', 'Baseline'),
              ('R4-fourload', '4 load'),
              ('R5-fourload-window', '4 load + capacity'),
              ('R1-frontend', 'Ideal dual frontend'),
              ('R4-fourload-frontend', 'Frontend + 4 load'),
              ('R5-fourload-window-frontend',
               'Frontend + 4 load + capacity')]
    values = []
    for key, label in groups:
        score = runs[key]['score']
        assert score['points'] == 145 and not score['provisional']
        values.append(score['INT_per_GHz'])
    fig, ax = plt.subplots(figsize=(10, 4.5), constrained_layout=True)
    fig.patch.set_facecolor('#fbfcfe')
    ax.set_facecolor('#fbfcfe')
    colors = ['#a8b4c2', '#8aaeca', '#689abe', '#4e8cb7',
              '#337ca7', '#17678c']
    ax.barh(range(len(groups)), values, color=colors, height=0.61)
    ax.set_yticks(range(len(groups)), [label for _, label in groups])
    ax.invert_yaxis()
    ax.set_xlim(0, 35.8)
    ax.set_xlabel('SPEC06 INT / GHz (0.3c measured)\n'
                  'Same 145 points; 20M warmup + 20M ROI', labelpad=10)
    ax.set_title('12-wide CPU: supply, load bandwidth and capacity',
                 fontsize=14, loc='left', pad=18)
    for y, value in enumerate(values):
        gain = (value / values[0] - 1) * 100
        label = f'{value:.2f}' + (f'  (+{gain:.2f}%)' if y else '')
        ax.text(value + 0.28, y, label, va='center', fontsize=10)
    ax.xaxis.grid(True, color='#dde4ec', linewidth=0.7)
    ax.set_axisbelow(True)
    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.tick_params(axis='both', length=0, labelsize=10)
    fig.savefig(root / 'figures' / 'summary-twelve-wide.png', dpi=180)
    plt.close(fig)


if __name__ == '__main__':
    main()
