import re
import sys
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('file', help='File to check')

args = parser.parse_args()

patterns = ['Invalid read', 'Use of uninitialised value', 'Conditional jump or move']
patterns = [re.compile(p) for p in patterns]

for ln, line in enumerate(open(args.file, 'r')):
    for pattern in patterns:
        if pattern.search(line):
            print(f"In valgrind log file {args.file}, line {ln}, found {pattern.pattern}, check failed!")
            sys.exit(-1)

print(f"Check passed!")