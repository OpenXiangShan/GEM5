#!/usr/bin/env python3

import sqlite3 as sql
import argparse

stages = ['f','d','r','D','i','a','g','e','b','w','c']

def non_stage():
    return '.'

def stage(x):
    return stages[x]

def dump_visual(pos, records, tick_per_cycle, cycle_per_line):
    pos_start = pos[0] % cycle_per_line
    line = ''
    line += '[' + non_stage() * pos_start
    pos_next = pos_start

    for i in range(1, len(pos)):
        if (pos[i] == pos[i-1]) or pos[i] == 0:
            pos[i] = pos[i-1]
            continue
        if (pos[i] < pos[i-1]):
            print("error: metaBuffer maybe overflow")
            continue
        if pos[i] - pos[i-1] >= cycle_per_line - pos_next:
            diff = cycle_per_line - pos_next
            line += f'{stage(i-1)}' * diff + ']\n'
            diff_line = ((pos[i] - pos[i-1]) - diff - 1) // cycle_per_line
            if diff_line > 0:
                line += '[' + f'{stage(i-1)}' * cycle_per_line + ']\n'

            pos_next = pos[i] % cycle_per_line
            line += '[' + f'{stage(i-1)}' * pos_next
        else:
            diff = pos[i] - pos[i-1]
            pos_next = pos[i] % cycle_per_line
            line += f'{stage(i-1)}' * diff

    if cycle_per_line - pos_next == 0:
        line += ']\n'
        line += f'[{stage(i)}{non_stage() * (cycle_per_line - 1)}]\n'
    else:
        line += f'{stage(i)}' + non_stage() * (cycle_per_line - pos_next - 1) + ']'

    line += str(records)
    print(line)

def dump_txt(pos, records):
    for i in range(len(pos)):
        print(f'{stage(i)}{pos[i]}', end=' ')
    print(records)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Paginated Performance CCT Viewer')
    parser.add_argument('sqldb', help='SQLite database file')
    parser.add_argument('-v', '--visual', action='store_true', default=False,
                       help='Use visual output mode')
    parser.add_argument('-z', '--zoom', action='store', type=float, default=1,
                       help='Zoom factor for cycle_per_line')
    parser.add_argument('-p', '--period', action='store', default=333,
                       help='Tick per cycle')
    parser.add_argument('--page-size', action='store', type=int, default=None,
                       help='Number of records per page')
    parser.add_argument('--page', action='store', type=int, default=None,
                       help='Page number to display (1-based)')

    args = parser.parse_args()

    tick_per_cycle = int(args.period)
    cycle_per_line = int(100 * args.zoom)

    # Check if pagination is requested
    use_pagination = args.page_size is not None or args.page is not None

    if use_pagination:
        # Set defaults for pagination
        page_size = args.page_size or 10000
        page = args.page or 1

        # Calculate offset
        offset = (page - 1) * page_size

        with sql.connect(args.sqldb) as con:
            cur = con.cursor()

            # Get total record count
            cur.execute("SELECT COUNT(*) FROM LifeTimeCommitTrace")
            total_records = cur.fetchone()[0]
            total_pages = (total_records + page_size - 1) // page_size

            if page < 1 or page > total_pages:
                print(f"Invalid page number. Available pages: 1 to {total_pages}")
                exit(1)

            print(f"Page {page}/{total_pages} (Records {offset + 1}-{min(offset + page_size, total_records)})")
            print("=" * 60)

            # Get data for specified page
            cur.execute(f"SELECT * FROM LifeTimeCommitTrace LIMIT {page_size} OFFSET {offset}")

            # Get column names
            col_name = [i[0] for i in cur.description]
            col_name = col_name[1:]  # Skip first column
            col_name = [i.lower() for i in col_name]

            rows = cur.fetchall()

            for row in rows:
                row = row[1:]  # Skip first column
                pos = []
                records = []
                i = 0

                for val in row:
                    if col_name[i].startswith('at'):
                        pos.append(val // tick_per_cycle)
                    elif col_name[i].startswith('pc'):
                        if val < 0:
                            val = val + (1 << 64)
                        records.append(hex(val))
                    else:
                        records.append(val)
                    i += 1

                if args.visual:
                    dump_visual(pos, records, tick_per_cycle, cycle_per_line)
                else:
                    dump_txt(pos, records)
    else:
        # Original behavior - process entire file
        with sql.connect(args.sqldb) as con:
            cur = con.cursor()
            cur.execute("SELECT * FROM LifeTimeCommitTrace")

            # Get column names
            col_name = [i[0] for i in cur.description]
            col_name = col_name[1:]  # Skip first column
            col_name = [i.lower() for i in col_name]

            rows = cur.fetchall()

            for row in rows:
                row = row[1:]  # Skip first column
                pos = []
                records = []
                i = 0

                for val in row:
                    if col_name[i].startswith('at'):
                        pos.append(val // tick_per_cycle)
                    elif col_name[i].startswith('pc'):
                        if val < 0:
                            val = val + (1 << 64)
                        records.append(hex(val))
                    else:
                        records.append(val)
                    i += 1

                if args.visual:
                    dump_visual(pos, records, tick_per_cycle, cycle_per_line)
                else:
                    dump_txt(pos, records)
