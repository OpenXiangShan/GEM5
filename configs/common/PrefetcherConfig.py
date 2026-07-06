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

def _configure_l2_composite(prefetcher, prefetcher_name, options):
    if options.kmh_align:
        assert prefetcher_name == 'L2CompositeWithWorkerPrefetcher'
        _configure_l2_composite_kmh_align(prefetcher)
    elif prefetcher_name == 'L2CompositeWithWorkerPrefetcher':
        _configure_l2_composite_default(prefetcher)

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

SOURCE_ADMISSION_L2_LEVELS = ('l2', 'l2_wrapper')
SOURCE_ADMISSION_L2_L3_LEVELS = SOURCE_ADMISSION_L2_LEVELS + ('l3',)


def _source_admission_override_prefix(cache_level):
    if cache_level == 'l1d':
        return 'l1d'
    if cache_level in SOURCE_ADMISSION_L2_L3_LEVELS:
        return 'l2_l3'
    return None


def _get_source_admission_option(options, cache_level, field):
    shared = getattr(options, 'source_pf_admission_{}'.format(field))
    prefix = _source_admission_override_prefix(cache_level)
    if prefix is None:
        return shared

    override = getattr(
        options,
        '{}_source_pf_admission_{}'.format(prefix, field),
        None)
    if override is not None:
        return override
    return shared


def _configure_source_admission(prefetcher, cache_level, options):
    enable_candidate_admission = (
        getattr(options, 'enable_source_pf_admission', False) and
        cache_level in SOURCE_ADMISSION_L2_LEVELS)
    enable_hint_admission = (
        getattr(options, 'enable_pfahead_source_pf_admission', False) and
        cache_level in SOURCE_ADMISSION_L2_LEVELS)
    enable_pfq_admission = (
        getattr(options, 'enable_unified_pfq_source_pf_admission', False) and
        cache_level in SOURCE_ADMISSION_L2_L3_LEVELS)
    enable_l1d_admission = (
        getattr(options, 'enable_l1d_source_pf_admission', False) and
        cache_level == 'l1d')
    enable_l1d_pfahead_feedback = (
        getattr(options, 'enable_l1d_pfahead_downstream_reject_feedback',
                False) and
        cache_level == 'l1d')
    if not (enable_candidate_admission or enable_hint_admission or
            enable_pfq_admission or enable_l1d_admission or
            enable_l1d_pfahead_feedback):
        return
    if not hasattr(prefetcher, 'enable_source_admission'):
        return

    apply_l1d_pfahead_admission = getattr(
        options, 'enable_l1d_pfahead_source_pf_admission', False)

    prefetcher.enable_source_admission = (
        enable_candidate_admission or enable_hint_admission or
        enable_pfq_admission or enable_l1d_admission)
    prefetcher.source_admission_apply_to_candidates = \
        enable_candidate_admission or enable_l1d_admission
    prefetcher.source_admission_apply_to_hints = enable_hint_admission
    prefetcher.source_admission_apply_to_pfq = enable_pfq_admission
    prefetcher.source_admission_skip_pfahead_candidates = \
        enable_l1d_admission and not apply_l1d_pfahead_admission
    prefetcher.source_admission_epoch = _get_source_admission_option(
        options, cache_level, 'epoch')
    prefetcher.source_admission_init_level = _get_source_admission_option(
        options, cache_level, 'init_level')
    prefetcher.source_admission_min_probe_level = _get_source_admission_option(
        options, cache_level, 'min_probe_level')
    prefetcher.source_admission_high_conf_level = _get_source_admission_option(
        options, cache_level, 'high_conf_level')
    prefetcher.source_admission_hysteresis = _get_source_admission_option(
        options, cache_level, 'hysteresis')
    prefetcher.source_admission_pressure_pfq_pct = _get_source_admission_option(
        options, cache_level, 'pressure_pfq_pct')
    prefetcher.source_admission_rescue_interval = _get_source_admission_option(
        options, cache_level, 'rescue_interval')
    prefetcher.source_admission_rescue_level = _get_source_admission_option(
        options, cache_level, 'rescue_level')
    prefetcher.source_admission_unused_weight = _get_source_admission_option(
        options, cache_level, 'unused_weight')
    prefetcher.source_admission_drop_full_weight = _get_source_admission_option(
        options, cache_level, 'drop_full_weight')
    prefetcher.source_admission_min_issued = _get_source_admission_option(
        options, cache_level, 'min_issued')
    prefetcher.source_admission_min_useful = _get_source_admission_option(
        options, cache_level, 'min_useful')
    prefetcher.source_admission_down_streak_threshold = \
        _get_source_admission_option(
            options, cache_level, 'down_streak_threshold')
    prefetcher.source_admission_warmup_epochs = _get_source_admission_option(
        options, cache_level, 'warmup_epochs')
    prefetcher.source_admission_delayed_window_epochs = \
        _get_source_admission_option(
            options, cache_level, 'delayed_window_epochs')
    prefetcher.source_admission_hint_min_level = _get_source_admission_option(
        options, cache_level, 'hint_min_level')
    prefetcher.source_admission_hint_ignore_pressure_gate = \
        _get_source_admission_option(
            options, cache_level, 'hint_ignore_pressure_gate')
    if (enable_l1d_pfahead_feedback and
            hasattr(prefetcher,
                    'enable_l1d_pfahead_downstream_reject_feedback')):
        prefetcher.enable_l1d_pfahead_downstream_reject_feedback = True
        prefetcher.l1d_pfahead_feedback_init_level = \
            options.l1d_pfahead_feedback_init_level
        prefetcher.l1d_pfahead_feedback_min_level = \
            options.l1d_pfahead_feedback_min_level
        prefetcher.l1d_pfahead_feedback_min_samples = \
            options.l1d_pfahead_feedback_min_samples
        prefetcher.l1d_pfahead_feedback_reject_pct = \
            options.l1d_pfahead_feedback_reject_pct
        prefetcher.l1d_pfahead_feedback_recover_pct = \
            options.l1d_pfahead_feedback_recover_pct
        prefetcher.l1d_pfahead_feedback_down_streak_threshold = \
            options.l1d_pfahead_feedback_down_streak_threshold
        prefetcher.l1d_pfahead_feedback_up_streak_threshold = \
            options.l1d_pfahead_feedback_up_streak_threshold
        prefetcher.l1d_pfahead_feedback_rescue_interval = \
            options.l1d_pfahead_feedback_rescue_interval
        prefetcher.l1d_pfahead_feedback_rescue_level = \
            options.l1d_pfahead_feedback_rescue_level

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

    _configure_source_admission(prefetcher, cache_level, options)
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
