import argparse
from collections import Counter
import json
from pathlib import Path
import re


def check_trace(path, width=8, byte_budget=64):
    routed = {}
    block_counts = Counter()
    carried_block_counts = Counter()
    pending_normal_packets = {}
    closed_epochs = set()
    commits = []
    fusion_pcs = {}
    sends_per_tick = Counter()
    last_decode = -1
    last_commit = -1
    totals = Counter()
    for line in Path(path).open():
        match = re.match(r"\s*(\d+): \S+: (\S+) (.*)", line)
        if not match:
            continue
        tick, kind, detail = match.groups()
        fields = dict(re.findall(r"(\w+)=([^ ]+)", detail))
        if kind == "rearm-empty":
            assert not any(pending_normal_packets.values()), (
                path,
                fields,
                "rearm overtakes an in-flight normal packet",
            )
            if fields["block_valid"] == "1":
                block = (fields["epoch"], fields["block"])
                carried_block_counts[block] = int(fields["block_insts"])
            totals["empty_rearms"] += 1
        elif kind == "send":
            sends_per_tick[tick] += 1
            assert sends_per_tick[tick] <= width, (path, tick, "send width")
        elif kind == "packet":
            assert int(fields["size"]) <= width
            pending_normal_packets[int(fields["ready"])] = int(fields["size"])
        elif kind == "route":
            offered = int(fields["offered"], 0)
            valid = int(fields["valid"], 0)
            fast = int(fields["fast"], 0)
            normal = int(fields["normal"], 0)
            cycle = int(fields["cycle"])
            pending_normal_packets = {
                ready: size
                for ready, size in pending_normal_packets.items()
                if ready > cycle
            }
            pending_normal_packets[int(fields["ready"])] = normal
            assert not fast & normal
            assert fast | normal == (1 << int(fields["size"])) - 1
            assert not fast & ~valid
            assert not fast & ~offered
            assert fast & (fast + 1) == 0, (path, fields, "not a prefix")
            assert int(fields["ready"]) == int(fields["cycle"]) + 1
            epoch = fields["epoch"]
            assert not fast or epoch not in closed_epochs, (path, fields)
            if normal and epoch != "0":
                closed_epochs.add(epoch)
            if fields["normal_only"] == "1":
                closed_epochs.add(epoch)
            totals["packets"] += 1
            totals["mixed_packets"] += bool(fast and normal)
        elif kind == "routed":
            sequence = int(fields["seq"])
            assert sequence not in routed, (path, sequence, "double routing")
            assert int(fields["fast"]) + int(fields["normal"]) == 1
            assert fields["fast"] != "1" or fields["valid"] == "1"
            routed[sequence] = fields
            for label in ("fast", "normal", "valid"):
                totals[label] += int(fields[label])
            if fields["fast"] == "1":
                block = (fields["epoch"], fields["ftq"])
                block_counts[block] += 1
                assert (
                    block_counts[block] + carried_block_counts[block] <= width
                ), (path, block, "per-block budget reset on empty")
                start = int(fields["block_start"], 0)
                end = int(fields["end"], 0)
                assert start <= int(fields["pc"], 0) < end
                assert end <= int(fields["block_end"], 0)
                assert end - start <= byte_budget, (path, fields)
        elif kind in ("normal-enqueue", "normal-ready"):
            sequence = int(fields["seq"])
            assert routed[sequence]["normal"] == "1", (path, sequence)
            ready = int(routed[sequence]["ready"])
            assert int(fields["cycle"]) >= ready
            if kind == "normal-enqueue":
                assert int(fields["cycle"]) == ready
                pending_normal_packets.pop(ready, None)
                totals["normal_enqueued"] += 1
        elif kind == "decode":
            sequence = int(fields["seq"])
            assert sequence > last_decode, (path, sequence, "decode order")
            last_decode = sequence
            assert sequence in routed, (path, sequence, "decode without route")
            assert routed[sequence]["valid"] == "1"
            assert fields["fast"] == routed[sequence]["fast"]
        elif kind == "fusion":
            fusion_pcs[fields["first"]] = fields["next_pc"]
        elif kind == "commit":
            sequence = int(fields["seq"])
            assert sequence > last_commit, (path, sequence, "commit order")
            last_commit = sequence
            commits.append(fields["pc"])
            if fields["seq"] in fusion_pcs:
                commits.append(fusion_pcs[fields["seq"]])
    assert totals["packets"] > 0, (path, "no mask-routing packets")
    totals["committed_instructions"] = len(commits)
    totals["fast_blocks"] = len(block_counts)
    return dict(totals), commits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--width", type=int, default=8)
    parser.add_argument("--bytes", type=int, default=64)
    arguments = parser.parse_args()
    totals, _ = check_trace(arguments.trace, arguments.width, arguments.bytes)
    print(json.dumps(totals, indent=2))


if __name__ == "__main__":
    main()
