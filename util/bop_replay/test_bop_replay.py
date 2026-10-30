import unittest

from util.bop_replay.bop_replay import ReplayEvent, replay_student_decisions


class StudentDecisionTest(unittest.TestCase):
    def _event(self, **kwargs):
        values = dict(
            access_seq=1,
            replay_order=1,
            bop_name="system.l2.bop_small",
            bop_kind="small",
            tick=10,
            trigger_addr=0x1000,
            trigger_pc=0x800,
            trigger_has_pc=True,
            late=False,
            teacher_offset=1,
            teacher_issue_enabled=False,
            student_issue_enabled=True,
            student_selected_valid=True,
            student_selected_enable=True,
            student_selected_offset=2,
            selected_offset=2,
            issue_enabled=True,
            cqf_allowed=True,
        )
        values.update(kwargs)
        return ReplayEvent(**values)

    def test_student_can_issue_when_teacher_is_disabled(self):
        self.assertEqual(replay_student_decisions([self._event()]), [])

    def test_detects_wrong_selected_offset(self):
        mismatch = replay_student_decisions(
            [self._event(selected_offset=1)]
        )
        self.assertEqual(len(mismatch), 1)
        self.assertEqual(mismatch[0]["expected_offset"], 2)

    def test_second_rank_uses_own_student_qualification(self):
        second = self._event(
            replay_order=2, candidate_rank=2, teacher_issue_enabled=False,
            student_selected_offset=4, selected_offset=4,
        )
        self.assertEqual(replay_student_decisions([self._event(), second]), [])
        self.assertEqual(replay_student_decisions([
            self._event(), self._event(
                replay_order=2, candidate_rank=2,
                student_selected_offset=4, selected_offset=4,
                student_issue_enabled=False, student_selected_enable=False,
                issue_enabled=False,
            )
        ]), [])

    def test_teacher_cannot_enable_unqualified_second_rank(self):
        second = self._event(
            candidate_rank=2, teacher_issue_enabled=True,
            student_issue_enabled=False, student_selected_enable=False,
            student_selected_offset=4, selected_offset=4, issue_enabled=True,
        )
        mismatch = replay_student_decisions([second])
        self.assertEqual(len(mismatch), 1)
        self.assertFalse(mismatch[0]["expected_issue"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
