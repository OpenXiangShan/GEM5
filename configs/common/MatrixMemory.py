"""Configure the supported coherent Matrix ingress without changing topology."""

from m5.util import fatal


def configure_matrix_ingress(options, system):
    if not getattr(options, "matrix", False):
        return
    if (not options.caches or not options.l2cache or options.num_cpus != 1 or
            options.external_memory_system or options.memchecker):
        fatal("Matrix memory requires the validated single-core L1/shared-L2 "
              "coherent topology")
    if int(system.cache_line_size) != 64:
        fatal("Matrix memory plans require 64-byte cache lines")
    acquire = bool(options.matrix_rmw_acquire)
    retention = bool(options.matrix_rmw_retention)
    if retention and not acquire:
        fatal("Matrix RMW retention requires acquisition")
    for cpu in system.cpu:
        cpu.matrixRmwAcquire = acquire
    caches = (list(system.l2_caches) if options.classic_l2 else
              [cache_slice.inner_cache for wrapper in system.l2_wrappers
               for cache_slice in wrapper.slices])
    for cache in caches:
        ways = int(cache.assoc)
        limit = options.matrix_rmw_max_ways_per_set
        if limit == -1:
            limit = min(ways - 1, ways // 2 + 1)
        if not 0 <= limit < ways:
            fatal("Matrix RMW quota must leave at least one ordinary way")
        cache.matrix_rmw_acquire = acquire
        cache.matrix_rmw_retention = retention
        cache.matrix_rmw_max_ways_per_set = limit
        print(f"Matrix L2 RMW {cache.path()}: acquire={int(acquire)} "
              f"retention={int(retention)} max_ways_per_set={limit}")
    for bus in system.tol2bus_list:
        bus.enable_matrix_response_lane = True
        bus.matrix_response_max_per_cycle = 2
        bus.enable_matrix_transactions = True
        bus.matrix_transaction_entries = 64
        bus.matrix_tracked_requests = 512
        bus.matrix_rmw_caches = caches if acquire else []
    if not options.classic_l2:
        for wrapper in system.l2_wrappers:
            for cache_slice in wrapper.slices:
                cache_slice.enable_matrix_resources = True
