import os
import os.path as osp
import argparse

parser = argparse.ArgumentParser()
# add a positional argument
parser.add_argument('--tick', type=int, help='roi start tick', action='store', default=0)
parser.add_argument('--pc', type=str, help='observe_pc', action='store', default='0')
_choices_ = ['all', 'unseen', 'seen']
parser.add_argument('--show', type=str, help='observe_pc', action='store', default='unseen',
                    choices=_choices_ + [x + '_of_pc' for x in _choices_])
parser.add_argument('--db', type=str, help='db file', action='store')
parser.add_argument('-o', '--output', type=str, help='output file', action='store')
parser.add_argument('--show-global-delta', action='store_true')
parser.add_argument('-M', '--max-trace', type=int, help='max count of trace to analyze', action='store')
args = parser.parse_args()

db_path = '/no/where'
if args.db is None:
    all_subdirs = [d for d in os.listdir('../warmup_scripts') if osp.isdir(osp.join('../warmup_scripts', d))]
    lastest_subdir = max(all_subdirs, key=lambda x: osp.getmtime(osp.join('../warmup_scripts', x)))
    db_path = osp.join('../warmup_scripts', lastest_subdir, 'mem_trace.db')
else:
    db_path = args.db