import m5
from m5.objects import *
from common.Caches import *
from common import ObjectList


def _get_hwp(hwp_option):
    if hwp_option == None:
        return NULL
    hwpClass = ObjectList.hwp_list.get(hwp_option)
    return hwpClass()

def create_prefetcher(cpu, cache_level, options):
    prefetcher_attr = '{}_hwp_type'.format(cache_level)
    prefetcher_name = ''
    prefetcher = NULL
    if hasattr(options, prefetcher_attr):
        prefetcher_name = getattr(options, prefetcher_attr)
        prefetcher = _get_hwp(prefetcher_name)
        print(f"create_prefetcher at {cache_level}: {prefetcher_name}")

    if prefetcher == NULL:
        return NULL

    if cpu != NULL:
        prefetcher.registerTLB(cpu.mmu.dtb)

    if prefetcher_name == 'XSCompositePrefetcher':
        if options.l1d_enable_spp:
            prefetcher.enable_spp = True
        if options.l1d_enable_cplx:
            prefetcher.enable_cplx = True
        prefetcher.pht_pf_level = options.pht_pf_level
        prefetcher.short_stride_thres = options.short_stride_thres
        prefetcher.enable_temporal = not options.kmh_align
        prefetcher.fuzzy_stride_matching = False
        prefetcher.stream_pf_ahead = True

        prefetcher.enable_bop = not options.kmh_align
        prefetcher.bop_large.delay_queue_enable = True
        prefetcher.bop_large.bad_score = 10
        prefetcher.bop_small.delay_queue_enable = True
        prefetcher.bop_small.bad_score = 5

        prefetcher.queue_size = 128
        prefetcher.max_prefetch_requests_with_pending_translation = 128
        prefetcher.region_size = 64*16  # 64B * blocks per region

        prefetcher.berti.use_byte_addr = True
        prefetcher.berti.aggressive_pf = False
        prefetcher.berti.trigger_pht = True

        if options.ideal_cache:
            prefetcher.stream_pf_ahead = False
        if options.kmh_align:
            prefetcher.enable_berti = False
            prefetcher.enable_sstride = True
            prefetcher.enable_activepage = False
            prefetcher.enable_xsstream = True

    if cache_level == 'l2':
        if options.kmh_align:
            assert prefetcher_name == 'L2CompositeWithWorkerPrefetcher'
            prefetcher.enable_cmc = True
            prefetcher.enable_bop = True
            prefetcher.enable_cdp = False
        if options.l1_to_l2_pf_hint:
            prefetcher.queue_size = 64
            prefetcher.max_prefetch_requests_with_pending_translation = 128

    if cache_level == 'l3':
        if options.l2_to_l3_pf_hint:
            prefetcher.queue_size = 64
            prefetcher.max_prefetch_requests_with_pending_translation = 128

    return prefetcher
