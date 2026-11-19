#!/usr/bin/env python3
"""Normalize the simulator's fact stream into an indexed, disk-backed database."""

import csv
import hashlib
import sqlite3
from pathlib import Path

COLUMNS = (
    "event order tick cycle actor level request_id prefetch_id related_id pc "
    "vaddr paddr secure context_id requestor_id inst_seq op size result source "
    "reason ready_tick cache command address_space arrival_tick"
).split()
INTEGERS = set(
    (
        "order tick cycle level request_id prefetch_id related_id secure context_id "
        "requestor_id inst_seq size source ready_tick arrival_tick"
    ).split()
)
ADDRESSES = {"pc", "vaddr", "paddr"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def address(value):
    return f"0x{int(value, 0):016x}" if value else None


def line_address(value, size):
    return f"0x{int(value, 0) & ~(size - 1):016x}" if value else None


def load_trace(path, database):
    """Read a closed trace; preserve all events and missing/zero distinctions."""
    db = sqlite3.connect(database)
    try:
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA journal_mode=OFF")
        db.execute("PRAGMA temp_store=FILE")
        definitions = [
            f'"{c}" {"INTEGER" if c in INTEGERS else "TEXT"}' for c in COLUMNS
        ]
        db.execute(
            "CREATE TABLE events ("
            + ",".join(definitions)
            + ",pa_line TEXT,va_line TEXT)"
        )
        metadata = {}

        def data_lines(stream):
            for text in stream:
                if text.startswith("# "):
                    key, value = text[2:].strip().split("=", 1)
                    metadata[key] = value
                else:
                    yield text

        insert = (
            "INSERT INTO events VALUES ("
            + ",".join("?" for _ in range(len(COLUMNS) + 2))
            + ")"
        )
        count, previous_order, previous_tick, batch = 0, 0, -1, []
        with Path(path).open(newline="") as stream:
            reader = csv.DictReader(data_lines(stream))
            require(
                reader.fieldnames == COLUMNS,
                f"{path}: unexpected trace columns",
            )
            require(
                metadata.get("prefetch_trace_version") == "3",
                "Unsupported trace version",
            )
            size = int(metadata["block_size"])
            require(
                size > 0 and not size & (size - 1), "Invalid cache line size"
            )
            for row in reader:
                require(
                    None not in row and None not in row.values(),
                    f"Malformed event near row {count + 1}",
                )
                for key in INTEGERS:
                    row[key] = int(row[key]) if row[key] else None
                for key in ADDRESSES:
                    row[key] = address(row[key])
                require(
                    row["order"] > previous_order, "Event order must increase"
                )
                require(
                    row["tick"] >= previous_tick,
                    "Trace ticks must not go backwards",
                )
                require(
                    row["request_id"]
                    or not row["event"].startswith("DEMAND_"),
                    "Demand lacks request identity",
                )
                previous_order, previous_tick = row["order"], row["tick"]
                batch.append(
                    [row[c] for c in COLUMNS]
                    + [
                        line_address(row["paddr"], size),
                        line_address(row["vaddr"], size),
                    ]
                )
                count += 1
                if len(batch) == 8192:
                    db.executemany(insert, batch)
                    batch.clear()
            if batch:
                db.executemany(insert, batch)
        require(
            metadata.get("trace_complete") == "1",
            "Trace is still open or truncated (no terminal marker)",
        )
        require(
            int(metadata["trace_end_tick"]) >= previous_tick,
            "Terminal tick precedes events",
        )
        db.executescript(
            """
            CREATE UNIQUE INDEX event_order ON events("order");
            CREATE INDEX event_pf ON events(prefetch_id,event,tick,"order");
            CREATE INDEX event_req ON events(cache,request_id,event);
            CREATE INDEX event_type ON events(event);
            CREATE INDEX event_training_instruction ON events(
                cache,inst_seq,context_id,address_space,pa_line)
                WHERE event='PF_TRAIN' AND level=1;
        """
        )
        db.commit()
        return db, {
            **metadata,
            "events": count,
            "trace": str(Path(path).resolve()),
            "sha256": digest(path),
        }
    except BaseException:
        db.close()
        raise


def export_csv(db, query, path, parameters=()):
    cursor = db.execute(query, parameters)
    with Path(path).open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow([column[0] for column in cursor.description])
        writer.writerows(cursor)
