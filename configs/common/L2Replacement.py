"""Apply replacement experiments only to the configured L2 caches."""

from m5.objects import (
    AMPMHintTable, AMPMPrefetcher, LRURP, PAMAIPRP, XSDRRIPRP,
)


def configure_l2_replacement(args, system):
    if not args.l2cache or args.l2_replacement_policy == "default":
        return
    for i in range(args.num_cpus):
        caches = ([system.l2_caches[i]] if args.classic_l2 else
                  [s.inner_cache for s in system.l2_wrappers[i].slices])
        hints = None
        external = args.ampm_hint_source == "prefetcher"
        if args.l2_replacement_policy == "ampm" or external:
            hints = AMPMHintTable(entries=args.ampm_map_entries,
                                  assoc=args.ampm_map_assoc,
                                  block_size=int(system.cache_line_size),
                                  external_hints=external)
            setattr(system, "l2_ampm_hints%d" % i, hints)
            if external:
                prefetcher = system.cpu[i].dcache.prefetcher
                if not isinstance(prefetcher, AMPMPrefetcher):
                    raise ValueError("AMPM hints require "
                                     "--l1d-hwp-type=AMPMPrefetcher")
                # AMPM returns vector candidates; it does not fill PFRequestBuffer.
                prefetcher.use_pf_buffer = False
                prefetcher.use_virtual_addresses = False
                prefetcher.prefetch_on_access = True
                prefetcher.prefetch_on_pf_hit = False
                prefetcher.prefetch_train = False
                prefetcher.on_inst = False
                prefetcher.on_miss = False
                prefetcher.on_data = True
                prefetcher.ampm.hot_zone_size = "4KiB"
                prefetcher.ampm.access_map_table_entries = str(args.ampm_map_entries)
                prefetcher.ampm.access_map_table_assoc = args.ampm_map_assoc
                prefetcher.ampm.replacement_hints = hints
        for cache in caches:
            if args.l2_replacement_policy == "lru":
                cache.replacement_policy = LRURP()
            elif args.l2_replacement_policy == "ampm":
                cache.replacement_policy = PAMAIPRP(
                    hints=hints,
                    low_insert_period=args.ampm_low_insert_period,
                    suppress_first_hit=not args.ampm_promote_first_hit,
                    protect_demand=args.ampm_protect_demand)
            else:
                assoc = int(cache.assoc)
                block_size = int(system.cache_line_size)
                cache.replacement_policy = XSDRRIPRP(
                    mode=2, num_sets=int(cache.size) // (assoc * block_size),
                    num_ways=assoc)
