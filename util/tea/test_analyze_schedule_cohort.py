#!/usr/bin/env python3
"""Check observed insertion, repeated scheduling, and signed pair offsets."""
import gzip
from pathlib import Path
import tempfile
import unittest

from analyze_schedule_cohort import analyze

TRACE = """130: cpu.scheduler: [sn:10] scheduler insert: producer
130: cpu.scheduler.intIQ0: [tid:0] [sn:10] IntAlu insert into intIQ0
132: cpu.scheduler: [sn:11] scheduler insert: consumer
132: cpu.scheduler.intIQ1: [tid:0] [sn:11] IntBr insert into intIQ1
140: cpu.scheduler.intIQ0: [sn:10] add to readyInstsQue
150: cpu.scheduler.intIQ0: [sn 10] was selected
150: cpu.scheduler.intIQ0: [sn:10] no conflict, scheduled
155: cpu.scheduler.intIQ0: [sn:10] arbitration failed, retry
160: cpu.scheduler.intIQ0: [sn:10] no conflict, scheduled
170: cpu.scheduler: IntAlu [sn:10] add to FUs
180: cpu.scheduler.intIQ1: [sn:11] no conflict, scheduled
O3PipeView:fetch:100:0x10:0:10:producer
O3PipeView:decode:110
O3PipeView:rename:0
O3PipeView:dispatch:0
O3PipeView:issue:170
O3PipeView:complete:190
O3PipeView:retire:210
O3PipeView:fetch:102:0x12:0:11:consumer
O3PipeView:decode:112
O3PipeView:rename:0
O3PipeView:dispatch:0
O3PipeView:issue:185
O3PipeView:complete:195
O3PipeView:retire:211
O3PipeView:fetch:101:0x10:0:12:squashed
O3PipeView:retire:0
O3PipeView:fetch:300:0x10:0:13:outside
O3PipeView:retire:330
"""


class ScheduleCohortTest(unittest.TestCase):
    def run_trace(self, text):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'trace.gz'
            with gzip.open(trace, 'wt') as stream:
                stream.write(text)
            return analyze(trace, 10, 100, 200, {0x10, 0x12, 0x14})

    def test_actual_insert_and_repeated_schedules(self):
        result = self.run_trace(TRACE)
        self.assertEqual(result['retired_selected_instructions'], 2)
        producer = result['PCs']['0x10']
        timing = producer['timing_cycles']
        self.assertEqual(timing['first_insert_to_issue']['mean'], 4)
        self.assertEqual(timing['first_scheduled_to_last_scheduled']['mean'], 1)
        self.assertEqual(producer['Schedule_log_calls']['retry'], 1)
        self.assertEqual(producer['IQ_distribution'], {'intIQ0': 1})
        self.assertEqual(result['PCs']['0x14']['timing_cycles'], {})

    def test_missing_event_and_speculative_offset(self):
        result = self.run_trace(TRACE)
        consumer = result['PCs']['0x12']
        self.assertEqual(consumer['missing_event_instructions']['ready'], 1)
        self.assertNotIn(
            'first_ready_to_first_scheduled', consumer['timing_cycles'])
        pair = result['adjacent_sequence_pairs']['0x10->0x12']
        self.assertEqual(
            pair['producer_complete_to_consumer_first_scheduled']['mean'], -1)

    def test_ambiguous_scalar_identity_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unique scalar'):
            self.run_trace(TRACE.replace('0x12:0:11:', '0x12:0:10:'))


if __name__ == '__main__':
    unittest.main()
