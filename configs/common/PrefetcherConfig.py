import m5
from m5.objects import *
from common.Caches import *
from common import ObjectList
from m5.objects.Prefetcher import XSPhysicalSmallBOP, XSVirtualLargeBOP


def _get_hwp(hwp_option):
    if hwp_option is None:
        return NULL
    hwpClass = ObjectList.hwp_list.get(hwp_option)
    return hwpClass()

def is_pf_buffer_enabled(options):
    # Enabled by default; --disable-pf-buffer is the only CLI override.
    return getattr(options, 'enable_pf_buffer', True)

def _configure_pf_buffer(prefetcher, pf_buffer_enabled):
    if prefetcher != NULL and hasattr(prefetcher, 'use_pf_buffer'):
        prefetcher.use_pf_buffer = pf_buffer_enabled

def _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled):
    # These controls are disabled when the pf-buffer handles filtering.
    if hasattr(prefetcher, 'prefetch_train'):
        prefetcher.prefetch_train = not pf_buffer_enabled
    if hasattr(prefetcher, 'queue_filter'):
        prefetcher.queue_filter = not pf_buffer_enabled

def _register_prefetcher_tlb(prefetcher, cpu):
    if cpu != NULL:
        prefetcher.registerTLB(cpu.mmu.dtb, cpu.mmu.functional)

def _configure_xs_composite_common(prefetcher, options):
    # Keep only option/profile-dependent overrides here. Stable model defaults
    # belong to XSCompositePrefetcher in Prefetcher.py.
    prefetcher.short_stride_thres = getattr(options, "short_stride_thres", 0)

    if options.ideal_cache:
        prefetcher.stream_pf_ahead = False

def _configure_xs_composite_default(prefetcher, options):
    prefetcher.enable_activepage = True
    prefetcher.enable_pht = True
    prefetcher.enable_berti = True
    prefetcher.enable_bop = False
    prefetcher.enable_temporal = True
    prefetcher.enable_sstride = False
    prefetcher.enable_xsstream = False
    prefetcher.enable_opt = False
    prefetcher.pht_pf_level = options.pht_pf_level

def _configure_xs_composite_kmh_align(prefetcher):
    prefetcher.enable_activepage = False
    prefetcher.enable_pht = True
    prefetcher.enable_berti = False
    prefetcher.enable_bop = False
    prefetcher.enable_temporal = False
    prefetcher.enable_sstride = True
    prefetcher.enable_xsstream = True
    prefetcher.enable_opt = False
    prefetcher.pht_pf_level = 2

def _configure_xs_composite(prefetcher, options, pf_buffer_enabled):
    _configure_xs_composite_common(prefetcher, options)

    # Start from the selected XSComposite profile, then apply explicit overrides.
    if options.kmh_align:
        _configure_xs_composite_kmh_align(prefetcher)
    else:
        _configure_xs_composite_default(prefetcher, options)

    if options.l1d_enable_spp:
        prefetcher.enable_spp = True
    if options.l1d_enable_cplx:
        prefetcher.enable_cplx = True

    _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled)

def _configure_l2_composite_default(prefetcher):
    # Normal L2CompositeWithWorker profile.
    prefetcher.enable_bop = True
    prefetcher.enable_cdp = True
    prefetcher.enable_cmc = False
    prefetcher.enable_despacito_stream = True

def _configure_l2_composite_kmh_align(prefetcher):
    # RTL-aligned L2CompositeWithWorker profile.
    prefetcher.enable_cmc = True
    prefetcher.enable_bop = True
    prefetcher.enable_cdp = False
    prefetcher.enable_despacito_stream = False
    prefetcher.bop_large = XSVirtualLargeBOP(is_sub_prefetcher=True,
                                             enable_adaptoffset=False)
    prefetcher.bop_small = XSPhysicalSmallBOP(is_sub_prefetcher=True,
                                              enable_adaptoffset=False)

def _configure_l2_bop_validation_defaults(prefetcher):
    for bop in (prefetcher.bop_large, prefetcher.bop_small):
        bop.enable_pc_validation_confidence = True
        bop.pc_validation_entries = 128
        bop.rr_eviction_validation_entries = 16384
        bop.pc_validation_miss_decay_period = 4
        bop.pc_validation_low_entry_miss_streak_threshold = 0
        bop.enable_global_bop_coverage_guard = True
        bop.global_bop_min_resolved_coverage_shift = 3

def _configure_l2_composite(prefetcher, prefetcher_name, options):
    if options.kmh_align:
        assert prefetcher_name == 'L2CompositeWithWorkerPrefetcher'
        _configure_l2_composite_kmh_align(prefetcher)
    elif prefetcher_name == 'L2CompositeWithWorkerPrefetcher':
        _configure_l2_composite_default(prefetcher)

    if prefetcher_name == 'L2CompositeWithWorkerPrefetcher':
        _configure_l2_bop_validation_defaults(prefetcher)

def _configure_l2_prefetcher(prefetcher, prefetcher_name, options,
                             pf_buffer_enabled):
    # classic_l2 attaches the real L2 prefetcher directly to the L2 cache.
    # Aligned L2 uses this level only as a forwarder to l2_wrapper.
    if options.classic_l2:
        _configure_l2_composite(prefetcher, prefetcher_name, options)
        _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled)
        if options.l1_to_l2_pf_hint:
            prefetcher.queue_size = 64
            prefetcher.max_prefetch_requests_with_pending_translation = 128
    else:
        assert prefetcher_name == 'PrefetcherForwarder'

def _configure_l2_wrapper_prefetcher(prefetcher, prefetcher_name, options,
                                     pf_buffer_enabled):
    # Aligned L2 attaches the real L2 prefetcher to l2_wrapper.
    # Classic L2 has no wrapper-level real prefetcher.
    if not options.classic_l2:
        _configure_l2_composite(prefetcher, prefetcher_name, options)
        _set_pf_buffer_training_policy(prefetcher, pf_buffer_enabled)
        if options.l1_to_l2_pf_hint:
            prefetcher.queue_size = 32
            prefetcher.max_prefetch_requests_with_pending_translation = 128

def _configure_l3_prefetcher(prefetcher, options):
    if options.l2_to_l3_pf_hint:
        prefetcher.queue_size = 64
        prefetcher.max_prefetch_requests_with_pending_translation = 128

def create_prefetcher(cpu, cache_level, options):
    prefetcher_attr = '{}_hwp_type'.format(cache_level)
    prefetcher_name = ''
    prefetcher = NULL
    pf_buffer_enabled = is_pf_buffer_enabled(options)
    if hasattr(options, prefetcher_attr):
        prefetcher_name = getattr(options, prefetcher_attr)
        prefetcher = _get_hwp(prefetcher_name)
        print(f"create_prefetcher at {cache_level}: {prefetcher_name}")

    _configure_pf_buffer(prefetcher, pf_buffer_enabled)

    if prefetcher == NULL:
        return NULL

    _register_prefetcher_tlb(prefetcher, cpu)

    if prefetcher_name == 'XSCompositePrefetcher':
        _configure_xs_composite(prefetcher, options, pf_buffer_enabled)

    if cache_level == 'l2':
        _configure_l2_prefetcher(prefetcher, prefetcher_name, options,
                                 pf_buffer_enabled)

    if cache_level == 'l2_wrapper':
        _configure_l2_wrapper_prefetcher(prefetcher, prefetcher_name, options,
                                         pf_buffer_enabled)

    if cache_level == 'l3':
        _configure_l3_prefetcher(prefetcher, options)

    return prefetcher
