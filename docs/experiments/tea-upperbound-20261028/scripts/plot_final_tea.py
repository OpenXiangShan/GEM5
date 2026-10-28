#!/usr/bin/env python3
"""Render mutually exclusive TEA states for matched final-configuration ROIs."""

import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    root = Path(__file__).resolve().parents[1]
    states = ['Computing', 'Stalled', 'Flushed', 'Drained']
    colors = ['#4a8eb8', '#ca786c', '#d6ad56', '#9ba7b4']
    for path in sorted((root / 'data').glob('final-tea-*.json')):
        result = json.loads(path.read_text())
        sides = result['sides']
        assert all(side['window']['kind'] == 'post-warmup ROI'
                   for side in sides)
        fig, ax = plt.subplots(figsize=(7.2, 4.8), constrained_layout=True)
        bottom = [0, 0]
        for state, color in zip(states, colors):
            values = [side['state_cpi'][state] for side in sides]
            ax.bar([0, 1], values, bottom=bottom, label=state,
                   color=color, width=0.56)
            bottom = [a + b for a, b in zip(bottom, values)]
        ax.set_xticks([0, 1], ['Original 12-wide', 'Final 12-wide'])
        ax.set_ylabel('CPI; mutually exclusive global states')
        ax.set_ylim(0, max(bottom) * 1.3)
        ax.set_title(result['name'] + ': matched 20M ROI', pad=14)
        for index, total in enumerate(bottom):
            ax.text(index, total + max(bottom) * 0.025,
                    f"IPC {sides[index]['ipc']:.3f}",
                    ha='center', fontsize=10)
        ax.legend(loc='upper right', ncol=4, fontsize=9, frameon=False)
        ax.spines[['top', 'right']].set_visible(False)
        fig.savefig(root / 'figures' /
                    ('final-tea-' + result['name'] + '.png'), dpi=180)
        plt.close(fig)


if __name__ == '__main__':
    main()
