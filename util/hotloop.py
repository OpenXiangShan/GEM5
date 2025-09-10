import re
from collections import Counter
import sys
import gzip

def parse_instruction(line):
    # something like [c1000 'c_addw a4 a5', '0x800000']
    match = re.search(r"c(\d+) \['([^']+)',\s*'([^']+)'\]", line)
    if match:
        return match.groups()   # commit time + instruction + pc
    return None

def is_branch_instruction(instr):
    branch_instructions = ['beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu', 'beqz', 'bnez', 'j', 'jal', 'jalr', 'ret', 'mret', 'sret']
    branch_instructions += ['c_beqz', 'c_bnez', 'c_j', 'c_jal', 'c_jr', 'c_jalr']
    return any(instr.split()[0].startswith(branch) for branch in branch_instructions)

def analyze_commit_trace(file_path):
    basic_blocks = Counter()
    basic_blocks_commit_time = Counter()
    current_block = []
    block_begin_commit_time = 0

    open_func = gzip.open if file_path.endswith('.gz') else open
    mode = 'rt' if file_path.endswith('.gz') else 'r'

    with open_func(file_path, mode) as file:
        commit_time = 0
        for line in file:
            parsed = parse_instruction(line) # commit time[0] + instruction[1] + pc[2]
            if parsed:
                if len(current_block) == 0:
                    block_begin_commit_time = int(parsed[0])
                commit_time = int(parsed[0])
                current_block.append((parsed[1], parsed[2]))
                if is_branch_instruction(parsed[1]):
                    if current_block:
                        # print(current_block)
                        basic_blocks[tuple(current_block)] += 1
                        basic_blocks_commit_time[tuple(current_block)] += int(parsed[0]) - block_begin_commit_time
                        current_block = []

        if current_block:
            basic_blocks[tuple(current_block)] += 1
            basic_blocks_commit_time[tuple(current_block)] += commit_time - block_begin_commit_time
    return basic_blocks, basic_blocks_commit_time

def main(file_path):
    basic_blocks, basic_blocks_commit_time = analyze_commit_trace(file_path)

    print("Top 10 most common basic blocks:")
    total_count = sum(basic_blocks.values())
    total_cycle = sum(basic_blocks_commit_time.values())
    print(f"Total cycle: {total_cycle}")
    for block, count in basic_blocks.most_common(10):
        percentage = (count / total_count) * 100
        print(f"Count: {count} ({percentage:.2f}%) cycle: {basic_blocks_commit_time[block]}", end='')
        print(f" ratio ({basic_blocks_commit_time[block] / total_cycle * 100:.2f}%)")
        print("Instructions:")
        for instr, pc in block:
            print(f"  {pc}: {instr}")
        print()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Please provide the file path as a command line argument.")
        sys.exit(1)
    file_path = sys.argv[1]
    main(file_path)