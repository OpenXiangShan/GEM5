import pickle
import numpy as np
import argparse

parser = argparse.ArgumentParser(
    description='Read and process a .pkl file for optimization results.'
)

parser.add_argument(
    'path', type=str, help='Path to the .pkl file to read'
)

args = parser.parse_args()

path = args.path

with open(path, 'rb') as f:
    checkpoint = pickle.load(f)
    start_params = checkpoint['params']
    start_score = checkpoint['score']

    tmp = []

    for i in range(len(start_params)):
        tmp.append({
            'params': [int(p) if isinstance(p, np.integer) else p for p in start_params[i]],
            'score': abs(start_score[i])
        })
    tmp.sort(key=lambda x: x['score'], reverse=True)
    

    print(f"{'ID':<6}{'score':<15}Parameters")
    print("-" * 80)
    

    for i in range(len(tmp)):
        print(f"{i:<6}{tmp[i]['score']:<15.3f}{tmp[i]['params']}")