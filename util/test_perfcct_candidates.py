import json
from pathlib import Path
import tempfile
import unittest

import perfcct_candidates as analysis


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.case = Path(self.tmp.name) / "case"
        self.case.mkdir()
        for side in ("off", "on"):
            (self.case / side).mkdir()
            command = {
                "command": [
                    "gem5.opt",
                    "--warmup-insts-no-switch=5000000",
                    "--maxinsts=10000000",
                ],
                "binary_sha256": "bin",
                "reference_sha256": "ref",
            }
            (self.case / side / "command.json").write_text(json.dumps(command))
        run = {
            "status": "passed",
            "host_exit_status": 0,
            "difftest_enabled": True,
            "reference_initialized": True,
            "stats_sections": 2,
            "errors": [],
            "maxinst_exit_ticks": [200],
            "trace_end_tick": 200,
        }
        self.validation = {
            "status": "passed",
            "off": run,
            "on": run,
            "timing_invariance": {
                "status": "passed",
                "difference_counts": [0, 0],
            },
        }
        self.diagnosis = {
            "query_window": {"start_tick": 100, "end_tick": 200},
            "roi_outside_known_trace": False,
            "examples": [],
        }
        (self.case / "off" / "config.ini").write_text(
            "[system.cpu.mmu.dtb]\nsize=48\n[system.cpu.dcache]\nmshrs=16\n"
        )
        self.write_stats(10, 5)

    def tearDown(self):
        self.tmp.cleanup()

    def write_stats(self, misses, rejects):
        (self.case / "off" / "stats.txt").write_text(
            "---------- Begin Simulation Statistics ----------\n"
            f"finalTick 200\nsimTicks 100\nsystem.cpu.mmu.dtb.misses {misses}\n"
            f"system.cpu.dcache.blockedCauses::no_mshrs {rejects}\n"
            "---------- End Simulation Statistics ----------\n"
        )

    def run_analysis(self):
        (self.case / "diagnose.json").write_text(json.dumps(self.diagnosis))
        (self.case / "validation.json").write_text(json.dumps(self.validation))
        return analysis.analyze(self.case)

    def example(
        self,
        seq,
        source="commit_blocker",
        reason="translation",
        wake="translation_observed_complete",
    ):
        identity = {"Cpu": "system.cpu", "TID": 0, "SeqNum": seq}
        return {
            "source": source,
            "selection": "p50_duration",
            "route": {
                "evidence": {
                    "identity": identity,
                    "seed_window": {"start_tick": 110, "end_tick": 150},
                    "events": [
                        {
                            "ID": seq * 10,
                            "Tick": 110,
                            "Attempt": 1,
                            "Event": "wait_begin",
                            "Detail": reason,
                            "RelatedSeq": 0,
                        },
                        {
                            "ID": seq * 10 + 1,
                            "Tick": 130,
                            "Attempt": 1,
                            "Event": "wake",
                            "Detail": wake,
                            "RelatedSeq": 0,
                        },
                    ],
                }
            },
        }

    def test_branch_pc_aggregation_is_hex_and_whole_run(self):
        (self.case / "off" / "topMisPredicts.csv").write_text(
            "startPC,control_pc,count\n10,13398,3\n20,13398,4\n30,abc,2\n40,bad,-1\n"
        )
        context = analysis.branch_context(self.case / "off")
        self.assertEqual(context["top10"][0], {"PC": 0x13398, "count": 7})
        self.assertEqual(context["total_whole_run_mispredictions"], 9)
        self.assertEqual(context["status"], "partial")
        self.assertIn("not aligned", context["window"])

    def test_config_ignores_gem5_multiline_sql_and_preserves_capacity(self):
        config_path = self.case / "off" / "config.ini"
        config_path.write_text(
            "[system.arch_db]\ntable_cmds=CREATE TABLE a(\n"
            "CREATE TABLE b(\n);\n[system.cpu.dcache]\nmshrs=16\n"
            "[system.cpu.mmu.dtb]\nsize=48\n"
        )
        config = analysis.config_values(config_path)
        self.assertEqual(
            analysis.config_integer(config, "system.cpu.dcache", "mshrs"), 16
        )
        self.assertEqual(
            analysis.config_integer(config, "system.cpu.mmu.dtb", "size"), 48
        )

    def test_translation_requires_multiple_blockers_and_measurement_misses(
        self,
    ):
        self.diagnosis["examples"] = [self.example(1), self.example(2)]
        result = self.run_analysis()
        self.assertEqual(
            [p["id"] for p in result["interventions"]], ["dtlb_x4"]
        )
        self.assertEqual(result["interventions"][0]["value"], 192)
        self.write_stats(0, 5)
        self.assertEqual(self.run_analysis()["interventions"], [])
        self.write_stats(10, 5)
        self.diagnosis["examples"] = [self.example(1), self.example(1)]
        self.assertEqual(self.run_analysis()["interventions"], [])
        self.diagnosis["examples"] = [
            self.example(1, "symptom"),
            self.example(2, "symptom"),
        ]
        self.assertEqual(self.run_analysis()["interventions"], [])

    def test_incompatible_waits_do_not_create_capacity_experiment(self):
        self.diagnosis["examples"] = [
            self.example(1, wake="cache_hint"),
            self.example(2, wake="cache_hint"),
        ]
        result = self.run_analysis()
        self.assertEqual(result["interventions"], [])
        self.assertFalse(
            any(
                c["category"] == "store_or_load_translation"
                for c in result["candidates"]
            )
        )

    def test_completion_or_provenance_failure_blocks_recommendations(self):
        self.diagnosis["examples"] = [self.example(1), self.example(2)]
        self.validation["timing_invariance"]["difference_counts"] = [0, 1]
        result = self.run_analysis()
        self.assertFalse(result["execution_gate"]["eligible"])
        self.assertEqual(result["interventions"], [])
        self.validation["timing_invariance"]["difference_counts"] = [0, 0]
        command = json.loads((self.case / "on" / "command.json").read_text())
        command["binary_sha256"] = "other"
        (self.case / "on" / "command.json").write_text(json.dumps(command))
        self.assertEqual(self.run_analysis()["interventions"], [])

    def test_no_mshr_requires_actual_repeated_rejects_and_counter(self):
        for seq in (1, 2):
            example = self.example(
                seq, reason="cache_refill", wake="cache_hint"
            )
            example["route"]["cache"] = {
                "events": [
                    {
                        "ID": seq,
                        "Tick": 120,
                        "Event": "reject",
                        "BlockedMask": 1,
                        "RequestID": seq,
                        "Cache": "system.cpu.dcache",
                        "Detail": "blocked",
                    }
                ]
            }
            self.diagnosis["examples"].append(example)
        self.assertEqual(
            self.run_analysis()["interventions"][0]["id"], "l1d_mshrs_x2"
        )
        self.write_stats(10, 0)
        self.assertEqual(self.run_analysis()["interventions"], [])
        self.write_stats(10, 5)
        self.diagnosis["examples"] = self.diagnosis["examples"][:1]
        self.assertEqual(self.run_analysis()["interventions"], [])
        self.diagnosis["examples"][0]["route"]["cache"]["events"][0][
            "Event"
        ] = "blocked"
        self.assertFalse(
            any(
                c["category"] == "actual_mshr_admission_reject"
                for c in self.run_analysis()["candidates"]
            )
        )

    def test_at_most_three_candidates_and_two_recommendations(self):
        for seq in (1, 2):
            example = self.example(seq)
            example["route"]["cache"] = {
                "events": [
                    {
                        "ID": seq,
                        "Tick": 120,
                        "Event": "reject",
                        "BlockedMask": 1,
                        "Cache": "system.cpu.dcache",
                        "Detail": "blocked",
                    }
                ]
            }
            self.diagnosis["examples"].append(example)
        self.diagnosis["examples"].append(
            self.example(3, reason="cache_refill", wake="cache_hint")
        )
        self.diagnosis["dispatch_stall_reasons"] = {
            "top_non_nostall": [{"reason": "ControlRecovery", "count": 10}]
        }
        result = self.run_analysis()
        self.assertEqual(len(result["candidates"]), 3)
        self.assertEqual(len(result["interventions"]), 2)
        self.assertIn(
            "control_recovery", result["omitted_candidate_categories"]
        )
        self.assertTrue(
            all(
                c["root_cause_status"] == "not_established"
                for c in result["candidates"]
            )
        )

    def dispatch(self, main, fraction=0.4, secondary="ControlRecovery"):
        self.diagnosis["dispatch_stall_reasons"] = {
            "status": "available",
            "bins_match_total": True,
            "top_non_nostall": [
                {"reason": main, "count": 400, "fraction": fraction},
                {"reason": secondary, "count": 5, "fraction": 0.005},
            ],
        }

    def iq_example(self, seq, reason="port_busy"):
        example = self.example(seq)
        example["route"]["evidence"]["events"] = [
            {
                "ID": seq * 10,
                "Tick": 120,
                "Attempt": 1,
                "Event": "iq_issue_blocked",
                "Detail": "iq=ld0;reason=" + reason,
                "RelatedSeq": 0,
            }
        ]
        return example

    def test_minor_control_counter_does_not_displace_actual_iq_and_refill(
        self,
    ):
        self.dispatch("LoadL1Bound")
        self.diagnosis["examples"] = [
            self.example(1, reason="cache_refill", wake="cache_hint"),
            self.example(2, reason="cache_refill", wake="cache_hint"),
            self.example(3),
            self.example(4),
            self.iq_example(5),
            self.iq_example(6),
        ]
        result = self.run_analysis()
        categories = [c["category"] for c in result["candidates"]]
        self.assertEqual(categories[0], "repeated_cache_refill_wait")
        self.assertIn("iq_dependency_candidates", categories)
        self.assertNotIn("control_recovery", categories)
        self.assertTrue(
            any(
                a["category"] == "control_recovery" and not a["retained"]
                for a in result["candidate_selection_audit"]
            )
        )

    def test_store_primary_survives_translation_without_inferring_storebuffer_full(
        self,
    ):
        self.dispatch("StoreL1Bound", 0.64)
        self.diagnosis["examples"] = [self.example(1), self.example(2)]
        self.diagnosis["symptom_rankings"] = [
            {
                "PC": 100,
                "iq_to_fu": 9999,
                "execution_or_memory": 0,
                "rob_drain": 0,
            }
        ]
        store = self.example(3)
        store["route"]["evidence"]["events"] = [
            {
                "ID": 30,
                "Tick": 120,
                "Attempt": 1,
                "Event": "store_data_ready",
                "Detail": "executed",
                "RelatedSeq": 0,
            }
        ]
        self.diagnosis["examples"].append(store)
        stats_path = self.case / "off" / "stats.txt"
        stats_path.write_text(
            stats_path.read_text().replace(
                "---------- End Simulation Statistics",
                "system.cpu.iew.stallEvents::IQFull 9\n"
                "system.cpu.lsq.sbufferFullCycles 0\n---------- End Simulation Statistics",
            )
        )
        candidate = self.run_analysis()["candidates"][0]
        self.assertEqual(candidate["category"], "store_dispatch_or_completion")
        self.assertEqual(
            candidate["facts"]["measurement_counters"][
                "system.cpu.iew.stallEvents::IQFull"
            ],
            "9",
        )
        self.assertEqual(
            candidate["facts"]["measurement_counters"][
                "system.cpu.lsq.sbufferFullCycles"
            ],
            "0",
        )
        self.assertEqual(candidate["next_probe"]["kind"], "artifact_request")
        self.assertEqual(
            candidate["facts"]["upstream_iq_context"][
                "iq_dominant_symptom_pcs"
            ][0]["PC"],
            100,
        )
        self.assertIn("不等于 L1 或 StoreBuffer", candidate["unknowns"][0])

    def test_refill_tag_write_is_window_evidence_not_generic_dependency(self):
        self.dispatch("LoadL1Bound")
        self.diagnosis["examples"] = [
            self.iq_example(1, "replay_refill_tag_write"),
            self.iq_example(2, "replay_refill_tag_write"),
            self.example(3),
        ]
        result = self.run_analysis()
        tag = result["candidates"][0]
        self.assertEqual(tag["category"], "refill_tag_write_contention")
        self.assertEqual(
            tag["selection_evidence"],
            {
                "evidence_level": "observed_blocker_mechanism",
                "distinct_instances": 2,
            },
        )
        self.assertEqual(tag["next_probe"]["kind"], "artifact_request")
        self.assertEqual(tag["root_cause_status"], "not_established")
        # Evidence outside the seed cannot establish a local contention interval.
        for example in self.diagnosis["examples"][:2]:
            example["route"]["evidence"]["events"][0]["Tick"] = 90
        self.assertNotIn(
            "refill_tag_write_contention",
            [c["category"] for c in self.run_analysis()["candidates"]],
        )

    def test_dominant_control_and_scalar_execute_keep_their_investigation_entry(
        self,
    ):
        self.dispatch("ControlRecovery", 0.28, "LoadL1Bound")
        self.diagnosis["examples"] = [
            self.example(1, reason="cache_refill", wake="cache_hint")
        ]
        result = self.run_analysis()
        self.assertEqual(
            result["candidates"][0]["category"], "control_recovery"
        )
        self.dispatch("ScalarLongExecute", 0.1)
        self.diagnosis["examples"].append(self.iq_example(2))
        self.assertEqual(
            self.run_analysis()["candidates"][0]["category"],
            "iq_dependency_candidates",
        )

    def test_explicit_no_interventions_preserves_evidence_and_suppresses_all_proposals(
        self,
    ):
        self.diagnosis["examples"] = [self.example(1), self.example(2)]
        self.assertEqual(
            self.run_analysis()["interventions"][0]["id"], "dtlb_x4"
        )
        result = analysis.analyze(self.case, no_interventions=True)
        self.assertEqual(result["interventions"], [])
        self.assertFalse(result["intervention_recommendations_enabled"])
        self.assertTrue(
            all(
                c["next_probe"]["kind"] == "artifact_request"
                for c in result["candidates"]
            )
        )
        self.assertIn("本次建议：关闭；脚本不执行模拟器", analysis.markdown(result))
        self.assertNotIn("自动干预准入", analysis.markdown(result))

    def test_same_level_uses_returned_instance_count_before_category_name(
        self,
    ):
        candidates = [
            {
                "category": name,
                "selection_evidence": {
                    "evidence_level": "observed_blocker_mechanism",
                    "distinct_instances": count,
                },
            }
            for name, count in (
                ("refill_tag_write_contention", 2),
                ("repeated_cache_refill_wait", 6),
            )
        ]
        retained, audit = analysis.choose_candidates(
            candidates, {"reason": "LoadL1Bound"}
        )
        self.assertEqual(retained[0]["category"], "repeated_cache_refill_wait")
        self.assertEqual(audit[0]["distinct_instances"], 6)

    def test_arbitration_failed_and_canceled_are_window_evidence_but_success_is_not(
        self,
    ):
        example = self.example(1)
        example["route"]["evidence"]["events"] = [
            {
                "ID": index,
                "Tick": tick,
                "Attempt": 1,
                "Event": "iq_arbitration",
                "Detail": "iq=intIQ0;reason=" + reason,
                "RelatedSeq": 0,
            }
            for index, (tick, reason) in enumerate(
                (
                    (120, "failed"),
                    (121, "canceled"),
                    (122, "success"),
                    (90, "failed"),
                    (123, "failed_other"),
                ),
                1,
            )
        ]
        self.diagnosis["examples"] = [example]
        candidate = self.run_analysis()["candidates"][0]
        self.assertEqual(candidate["category"], "iq_dependency_candidates")
        self.assertEqual(
            [e["event_id"] for e in candidate["facts"]["window_decisions"]],
            [1, 2],
        )


if __name__ == "__main__":
    unittest.main()
