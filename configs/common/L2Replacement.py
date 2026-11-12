"""Apply replacement experiments only to the configured L2 caches."""

from m5.objects import AMPMHintTable, LRURP, PAMAIPRP, XSDRRIPRP


def configure_l2_replacement(args, system):
    if not args.l2cache or args.l2_replacement_policy == "default":
        return
    for i in range(args.num_cpus):
        caches = ([system.l2_caches[i]] if args.classic_l2 else
                  [s.inner_cache for s in system.l2_wrappers[i].slices])
        hints = None
        if args.l2_replacement_policy == "ampm":
            hints = AMPMHintTable(entries=args.ampm_map_entries,
                                  assoc=args.ampm_map_assoc,
                                  block_size=int(system.cache_line_size))
            setattr(system, "l2_ampm_hints%d" % i, hints)
        for cache in caches:
            if args.l2_replacement_policy == "lru":
                cache.replacement_policy = LRURP()
            elif args.l2_replacement_policy == "ampm":
                cache.replacement_policy = PAMAIPRP(
                    hints=hints,
                    low_insert_period=args.ampm_low_insert_period,
                    suppress_first_hit=not args.ampm_promote_first_hit)
            else:
                assoc = int(cache.assoc)
                block_size = int(system.cache_line_size)
                cache.replacement_policy = XSDRRIPRP(
                    mode=2, num_sets=int(cache.size) // (assoc * block_size),
                    num_ways=assoc)
