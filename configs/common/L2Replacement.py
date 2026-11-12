"""Apply replacement experiments only to the configured L2 caches."""

from m5.objects import LRURP, XSDRRIPRP


def configure_l2_replacement(args, system):
    if not args.l2cache or args.l2_replacement_policy == "default":
        return
    for i in range(args.num_cpus):
        caches = ([system.l2_caches[i]] if args.classic_l2 else
                  [s.inner_cache for s in system.l2_wrappers[i].slices])
        for cache in caches:
            if args.l2_replacement_policy == "lru":
                cache.replacement_policy = LRURP()
            else:
                assoc = int(cache.assoc)
                block_size = int(system.cache_line_size)
                cache.replacement_policy = XSDRRIPRP(
                    mode=2, num_sets=int(cache.size) // (assoc * block_size),
                    num_ways=assoc)
