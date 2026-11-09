#!/usr/bin/env python3
"""Check physical ownership snapshots, fusion records and unknown observations."""
import gzip
from pathlib import Path
import tempfile
import unittest

from analyze_schedule_dependencies import analyze

LOG = """95: cpu.scheduler: [sn:10] addProdecer
95: cpu.scheduler: mark scoreboard p5 not ready
95: cpu.scheduler: [sn:10] scheduler insert: fused address
100: cpu.scheduler: IntAlu [sn:10] add to FUs
110: cpu.scheduler.iq: [sn:20] src p5 add to depGraph
110: cpu.scheduler.iq: [sn:20] src p5 add to depGraph
120: cpu.scheduler.iq: [sn:20] add to readyInstsQue
130: cpu.scheduler: MemRead [sn:20] add to FUs
135: cpu.scheduler: [sn:11] addProdecer
135: cpu.scheduler: mark scoreboard p5 not ready
135: cpu.scheduler: [sn:11] scheduler insert: later owner
140: cpu.scheduler: IntAlu [sn:11] add to FUs
150: cpu.scheduler.iq: [sn:21] src p5 add to depGraph
160: cpu.scheduler.iq: [sn:21] add to readyInstsQue
170: cpu.scheduler: MemRead [sn:21] add to FUs
175: cpu.scheduler.iq: [sn:22] src p9 add to depGraph
180: cpu.scheduler: MemRead [sn:22] add to FUs
"""


def pipeline(sn, pc, fetch, issue, retire):
    return (f'O3PipeView:fetch:{fetch}:{hex(pc)}:0:{sn}:instruction\n'
            f'O3PipeView:issue:{issue}\n'
            f'O3PipeView:retire:{retire}\n')


TRACE = (LOG + pipeline(10, 0x10, 90, 0, 0)
         + pipeline(11, 0x12, 131, 140, 151)
         + pipeline(20, 0x20, 105, 130, 145)
         + pipeline(21, 0x20, 145, 170, 185)
         + pipeline(22, 0x20, 165, 180, 195))


class ScheduleDependencyTest(unittest.TestCase):
    def run_trace(self, text):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'trace.gz'
            with gzip.open(trace, 'wt') as stream:
                stream.write(text)
            return analyze(trace, 10, 100, 200, {0x20})

    def test_owner_is_captured_before_register_reuse(self):
        data = self.run_trace(TRACE)['PCs']['0x20']
        self.assertEqual(data['retired_consumer_instructions'], 3)
        self.assertEqual(data['observed_consumer_issue_times'], 3)
        self.assertEqual(data['dependency_log_calls'], 4)
        self.assertEqual(data['distinct_registered_physical_dependencies'], 3)
        self.assertEqual(data['producer_pipeline_PC_distribution'],
                         {'0x10': 1, '0x12': 1, 'unobserved': 1})

    def test_actual_fu_log_covers_fusion_and_preserves_unknowns(self):
        data = self.run_trace(TRACE)['PCs']['0x20']
        gap = data['edge_timing_cycles'][
            'producer_last_to_fu_to_consumer_last_to_fu']
        self.assertEqual(gap['count'], 2)
        self.assertEqual(gap['mean'], 3)
        self.assertEqual(data['missing_observations']['producer_to_fu'], 1)
        names = data['producer_scheduled_instruction_distribution']
        self.assertEqual(names['fused address'], 1)

    def test_interrupted_destination_context_is_rejected(self):
        text = TRACE.replace('95: cpu.scheduler: mark scoreboard',
                             '95: cpu.scheduler: unrelated event\n'
                             '95: cpu.scheduler: mark scoreboard', 1)
        with self.assertRaisesRegex(ValueError, 'producer context'):
            self.run_trace(text)

    def test_duplicate_selected_identity_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'ambiguous selected'):
            self.run_trace(TRACE + pipeline(20, 0x20, 105, 130, 145))


if __name__ == '__main__':
    unittest.main()
