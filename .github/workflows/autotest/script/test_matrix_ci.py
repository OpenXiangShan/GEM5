"""Acceptance checks must reject false-green simulator runs."""

from pathlib import Path
import tempfile
import unittest

import matrix_ci
from matrix_ci import evaluate


VALID = (
    "info: NEMU register state: 1544 bytes, FCSR comparison enabled\n"
    "[matrix-ci-dispatch] reference=0 capabilities=3 "
    "implementation=1 tactic=1 status=0\n"
    "[matrix-ci] result=PASS nmse=0 limit=1e-7 ulp=0 "
    "prepare=0 reference=0 optimized=0\n"
    "--- 1 passed, 0 failed, 0 skipped ---\n"
    "Exiting @ tick 100 because m5_exit instruction encountered "
    "when simulating XS\n"
    "info: MatrixDiff committed=6 submitted=4 completed=4 checked=4 mma=1 "
    "memorySent=8 memoryCompleted=8 pendingChecks=0 pendingOwners=0 "
    "pendingMemory=0\n"
)


class MatrixCiAcceptanceTest(unittest.TestCase):
    def check_failed(self, log, returncode=0, timed_out=False):
        self.assertFalse(
            evaluate(log, {"matrix_required": True}, returncode, timed_out)[
                "passed"
            ]
        )

    def test_matrix_success(self):
        self.assertTrue(
            evaluate(VALID, {"matrix_required": True}, 0, False)["passed"]
        )

    def test_numeric_failure_even_with_zero_process_status(self):
        self.check_failed(VALID.replace("result=PASS", "result=FAIL"))

    def test_reference_initialization_required(self):
        self.check_failed(VALID.replace("1544 bytes", "1376 bytes"))

    def test_matrix_fallback_rejected(self):
        self.check_failed(VALID.replace("capabilities=3", "capabilities=1"))

    def test_matrix_execution_required(self):
        self.check_failed(VALID.replace("mma=1", "mma=0"))

    def test_unchecked_work_rejected(self):
        self.check_failed(VALID.replace("checked=4", "checked=3"))

    def test_pending_memory_rejected(self):
        self.check_failed(VALID.replace("pendingMemory=0", "pendingMemory=1"))

    def test_missing_counter_groups_rejected(self):
        for group in ("submitted=4 completed=4 checked=4",
                      "memorySent=8 memoryCompleted=8"):
            with self.subTest(group=group):
                result = evaluate(VALID.replace(group, ""),
                                  {"matrix_required": True}, 0, False)
                self.assertFalse(result["passed"])
                self.assertEqual(result["instruction_difftest"], "incomplete")

    def test_instruction_limit_is_not_completion(self):
        self.check_failed(
            VALID.replace(
                "m5_exit instruction encountered when simulating XS",
                "a thread reached the max instruction count",
            )
        )

    def test_nonzero_guest_status_rejected(self):
        self.check_failed(
            VALID + "Simulated exit code not 0! Exit code is 1\n"
        )

    def test_timeout_rejected(self):
        self.check_failed(VALID, timed_out=True)

    def test_nonzero_simulator_status_rejected(self):
        self.check_failed(VALID, returncode=-6)

    def test_auxiliary_operator_can_have_zero_matrix_work(self):
        log = VALID.replace("capabilities=3", "capabilities=1").replace(
            "mma=1", "mma=0"
        )
        self.assertTrue(
            evaluate(log, {"matrix_required": False}, 0, False)["passed"]
        )

    def test_numeric_failure_does_not_imply_difftest_failure(self):
        result = evaluate(
            VALID.replace("result=PASS", "result=FAIL"),
            {"matrix_required": True},
            0,
            False,
        )
        self.assertFalse(result["application_correctness"])
        self.assertEqual(result["instruction_difftest"], "passed")

    def test_abort_leaves_numerical_correctness_unknown(self):
        log = "\n".join(
            line
            for line in VALID.splitlines()
            if "[matrix-ci] result=" not in line
            and "Exiting @" not in line
            and "MatrixDiff committed=" not in line
        )
        log += "\nwarn: NEMU PC: 0x1234, GEM5 PC: 0x1234, inst: vfwcvt_f_x_v_micro\n"
        log += "panic: Difftest failed!\n"
        result = evaluate(log, {"matrix_required": True}, -6, False)
        self.assertIsNone(result["application_correctness"])
        self.assertEqual(result["instruction_difftest"], "failed")
        self.assertIn("0x1234", result["first_divergence"])


class MatrixCiImagesTest(unittest.TestCase):
    def test_selected_image_needs_no_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "softmax-fp32-n65536-test.bin"
            image.write_bytes(b"image")
            images = matrix_ci.select_images(Path(directory), ["softmax"])
            self.assertEqual(len(images), 1)
            self.assertEqual(images[0]["image"], image)
            self.assertFalse(images[0]["matrix_required"])

    def test_missing_operator_cannot_shrink_full_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "softmax-fp32-n65536-test.bin"
            image.write_bytes(b"image")
            with self.assertRaisesRegex(ValueError, "Missing CI images"):
                matrix_ci.select_images(Path(directory))

    def test_unknown_case_cannot_be_silently_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "Unknown CI cases"):
                matrix_ci.select_images(Path(directory), ["sofmax"])


if __name__ == "__main__":
    unittest.main()
