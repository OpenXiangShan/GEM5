# Copyright (c) 2026 XiangShan
# SPDX-License-Identifier: BSD-3-Clause

import math

from m5.objects import FDPRP, LRURP, XSDRRIPRP


def set_l2_replacement(args, cache):
    """Apply an explicit L2 policy after the KMHV3 defaults are installed."""
    policy = args.l2_replacement_policy
    ways = int(cache.assoc)
    line_size = int(args.cacheline_size)
    sets = int(cache.size) // (line_size * ways)
    if policy == "lru":
        cache.replacement_policy = LRURP()
    elif policy == "xs-drrip":
        cache.replacement_policy = XSDRRIPRP(
            mode=2, num_sets=sets, num_ways=ways)
    elif policy == "fdp":
        # Match the paper's interval/cache-capacity ratio on each L2 slice.
        cache.replacement_policy = FDPRP(
            num_ways=ways, num_sets=sets,
            block_bits=int(math.log2(line_size)),
            interval_evictions=max(1, sets * ways // 2))
