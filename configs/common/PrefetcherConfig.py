import copy
import os
import runpy

import m5
from m5.objects import *
from m5.util import fatal
from common.Caches import *
from common import ObjectList
from m5.objects.Prefetcher import XSPhysicalSmallBOP, XSVirtualLargeBOP

PF_SOURCE_NAMES = [
    "PF_NONE",
    "SStream",
    "SStride",
    "SPht",
    "HWP_BOP",
    "SPP",
    "CMC",
    "IPCP",
    "IPCP_CS",
    "IPCP_CPLX",
    "Berti",
    "StoreStream",
    "CDP",
    "SOpt",
    "DespacitoStream",
]


def _normalize_pf_source_name(name):
    return name.replace("_", "").replace("-", "").lower()


PF_SOURCE_BY_NAME = {
    _normalize_pf_source_name(name): idx
    for idx, name in enumerate(PF_SOURCE_NAMES)
}
PF_SOURCE_BY_NAME.update({
    "bop": PF_SOURCE_BY_NAME["hwpbop"],
    "stream": PF_SOURCE_BY_NAME["sstream"],
    "stride": PF_SOURCE_BY_NAME["sstride"],
    "pht": PF_SOURCE_BY_NAME["spht"],
    "store": PF_SOURCE_BY_NAME["storestream"],
    "despacito": PF_SOURCE_BY_NAME["despacitostream"],
})

PF_CONTROL_CONFIG_ENV = "GEM5_PF_CONTROL_CONFIG"

# Central Python-only configuration for prefetch admission control.
# Batch scripts may point GEM5_PF_CONTROL_CONFIG at a Python file defining
# PF_CONTROL_CONFIG with the same nested shape to override these defaults.
PF_CONTROL_CONFIG = {
    "control": {
        "enabled": False,
        "window": 100000,
        "admit_pct": 100,
        "sweep": [],
        "source_admit_pcts": {},
        "sweep_windows": 1,
        "warmup_windows": 0,
    },
    "adaptive": {
        "enabled": False,
        "min_pct": 5,
        "pct_quantum": 10,
        "gradient_step": 10,
        "pfbad_weight": (3, 2),
        "dpf_min_samples": 1,
        "dpf_deadband": 0,
        "improve_margin_bps": 0,
        "history_fallback": True,
        "best_topk": 1,
        "table_entries": 32,
        "pfbad_entries": {
            "l1d": 128,
            "l2": 256,
            "l2_wrapper": 256,
        },
        "warmup_windows": 1,
        "max_source_step": 10,
    },
}

_LOADED_PF_CONTROL_CONFIG = None

_ENABLE_DYNAMIC_PF = False

_DYNAMIC_PF_DISABLED_CONFIG = {
    "control": {
        "enabled": False,
    },
    "adaptive": {
        "enabled": False,
    },
}

_DYNAMIC_PF_ENABLED_CONFIG = {
    "control": {
        "enabled": True,
        "window": 8000,
        "admit_pct": 100,
    },
    "adaptive": {
        "enabled": True,
        "pfbad_entries": {
            "l1d": 128,
            "l2": 512,
            "l2_wrapper": 512,
        },
    },
}


def _apply_dynamic_pf_config(config):
    if _ENABLE_DYNAMIC_PF:
        _deep_update_dict(config, _DYNAMIC_PF_ENABLED_CONFIG)
    else:
        _deep_update_dict(config, _DYNAMIC_PF_DISABLED_CONFIG)


def _finalize_dynamic_pf_config(config):
    control = config.setdefault("control", {})
    adaptive = config.setdefault("adaptive", {})
    control["enabled"] = _ENABLE_DYNAMIC_PF
    adaptive["enabled"] = _ENABLE_DYNAMIC_PF


def set_enable_dynamic_pf(enabled):
    global _ENABLE_DYNAMIC_PF
    global _LOADED_PF_CONTROL_CONFIG

    _ENABLE_DYNAMIC_PF = bool(enabled)
    _LOADED_PF_CONTROL_CONFIG = None


def _get_hwp(hwp_option):
    if hwp_option is None:
        return NULL
    hwpClass = ObjectList.hwp_list.get(hwp_option)
    return hwpClass()


def _deep_update_dict(base, override):
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            _deep_update_dict(base[key], value)
        else:
            base[key] = value


def _load_pf_control_config():
    global _LOADED_PF_CONTROL_CONFIG
    if _LOADED_PF_CONTROL_CONFIG is not None:
        return _LOADED_PF_CONTROL_CONFIG

    config = copy.deepcopy(PF_CONTROL_CONFIG)
    _apply_dynamic_pf_config(config)

    config_path = os.environ.get(PF_CONTROL_CONFIG_ENV, "").strip()
    if config_path:
        try:
            namespace = runpy.run_path(config_path)
        except OSError as exc:
            fatal(f"Cannot load {PF_CONTROL_CONFIG_ENV}={config_path}: {exc}")
        override = namespace.get("PF_CONTROL_CONFIG")
        if not isinstance(override, dict):
            fatal(
                f"{PF_CONTROL_CONFIG_ENV} file must define "
                "PF_CONTROL_CONFIG as a dict"
            )
        _deep_update_dict(config, override)

    _finalize_dynamic_pf_config(config)
    _LOADED_PF_CONTROL_CONFIG = config
    return config


def _config_error(path, requirement, value):
    fatal(f"PF_CONTROL_CONFIG[{path!r}] must be {requirement} (got {value!r})")


def _int_config(value, path):
    try:
        return int(value)
    except (TypeError, ValueError):
        _config_error(path, "an integer", value)


def _positive_int_config(config, name, default, path):
    value = _int_config(config.get(name, default), path)
    if value <= 0:
        _config_error(path, "> 0", value)
    return value


def _nonnegative_int_config(config, name, default, path):
    value = _int_config(config.get(name, default), path)
    if value < 0:
        _config_error(path, ">= 0", value)
    return value


def _percent_config(config, name, default, path):
    value = _int_config(config.get(name, default), path)
    if value < 0 or value > 100:
        _config_error(path, "in [0, 100]", value)
    return value


def _positive_percent_config(config, name, default, path):
    value = _percent_config(config, name, default, path)
    if value <= 0:
        _config_error(path, "in (0, 100]", value)
    return value


def _bps_config(config, name, default, path):
    value = _int_config(config.get(name, default), path)
    if value < 0 or value > 10000:
        _config_error(path, "in [0, 10000]", value)
    return value


def _parse_pct_list(value, path):
    if value in (None, ""):
        return []

    result = []
    items = value.split(',') if isinstance(value, str) else value
    for item in items:
        if isinstance(item, str):
            item = item.strip()
            if not item:
                continue
        pct = _int_config(item, path)
        if pct < 0 or pct > 100:
            _config_error(path, "all entries in [0, 100]", pct)
        result.append(pct)
    return result


def _parse_pf_source(name):
    try:
        value = int(name)
    except ValueError:
        key = _normalize_pf_source_name(name)
        if key not in PF_SOURCE_BY_NAME:
            valid = ",".join(PF_SOURCE_NAMES)
            fatal(
                f"Invalid prefetch source {name!r}; valid sources are "
                f"{valid} or numeric indexes"
            )
        return PF_SOURCE_BY_NAME[key]

    if value < 0 or value >= len(PF_SOURCE_NAMES):
        fatal(
            f"Prefetch source index must be in [0, {len(PF_SOURCE_NAMES) - 1}] "
            f"(got {value})"
        )
    return value


def _validate_source_pct(pct, path):
    value = _int_config(pct, path)
    if value < -1 or value > 100:
        _config_error(path, "in [-1, 100]", value)
    return value


def _parse_source_pct_table(spec, path):
    table = [-1] * len(PF_SOURCE_NAMES)
    if spec in (None, "", {}, []):
        return []

    if isinstance(spec, dict):
        items = spec.items()
    elif isinstance(spec, str):
        items = []
        for raw_pair in spec.split(','):
            pair = raw_pair.strip()
            if not pair:
                continue
            if '=' not in pair:
                _config_error(path, "source=pct entries", pair)
            raw_source, raw_pct = pair.split('=', 1)
            items.append((raw_source.strip(), raw_pct.strip()))
    elif isinstance(spec, (list, tuple)):
        if len(spec) == len(PF_SOURCE_NAMES):
            for idx, pct in enumerate(spec):
                table[idx] = _validate_source_pct(pct, f"{path}.{idx}")
            return table
        items = spec
    else:
        _config_error(path, "a dict, full list, pair list, or string", spec)

    for item in items:
        try:
            raw_source, raw_pct = item
        except (TypeError, ValueError):
            _config_error(path, "source/pct pairs", item)
        source = _parse_pf_source(str(raw_source).strip())
        table[source] = _validate_source_pct(raw_pct, f"{path}.{raw_source}")
    return table


def _parse_weight_ratio(spec):
    if isinstance(spec, (list, tuple)):
        if len(spec) != 2:
            _config_error("adaptive.pfbad_weight", "NUM/DEN", spec)
        raw_num, raw_den = spec
    else:
        text = str(spec).strip()
        if '/' in text:
            raw_num, raw_den = text.split('/', 1)
        else:
            raw_num, raw_den = text, '1'
    num = _int_config(raw_num, "adaptive.pfbad_weight.numer")
    den = _int_config(raw_den, "adaptive.pfbad_weight.denom")
    if num <= 0 or den <= 0:
        _config_error("adaptive.pfbad_weight", "positive NUM/DEN", spec)
    return num, den


def _adaptive_pfbad_entries_for_level(cache_level, adaptive_config):
    entries = adaptive_config.get("pfbad_entries", {})
    if not isinstance(entries, dict):
        value = _int_config(entries, "adaptive.pfbad_entries")
        if value < 0:
            _config_error("adaptive.pfbad_entries", ">= 0", value)
        return value
    if cache_level == 'l1d':
        return _nonnegative_int_config(
            entries, 'l1d', 128, "adaptive.pfbad_entries.l1d")
    if cache_level == 'l2':
        return _nonnegative_int_config(
            entries, 'l2', 256, "adaptive.pfbad_entries.l2")
    if cache_level == 'l2_wrapper':
        return _nonnegative_int_config(
            entries, 'l2_wrapper',
            entries.get('l2', 256),
            "adaptive.pfbad_entries.l2_wrapper")
    return 0


def _apply_pf_control(prefetcher, config):
    if prefetcher == NULL or not hasattr(prefetcher, 'pf_control'):
        return

    control = config.get("control", {})
    admit_pct = _percent_config(
        control, 'admit_pct', 100, "control.admit_pct")
    window = _positive_int_config(
        control, 'window', 100000, "control.window")
    sweep_windows = _positive_int_config(
        control, 'sweep_windows', 1, "control.sweep_windows")
    warmup_windows = _nonnegative_int_config(
        control, 'warmup_windows', 0, "control.warmup_windows")

    prefetcher.pf_control = bool(control.get('enabled', False))
    prefetcher.pf_control_window = window
    prefetcher.pf_control_admit_pct = admit_pct
    prefetcher.pf_control_sweep = _parse_pct_list(
        control.get('sweep', []), "control.sweep")
    prefetcher.pf_control_source_admit_pcts = _parse_source_pct_table(
        control.get('source_admit_pcts', {}),
        "control.source_admit_pcts")
    prefetcher.pf_control_sweep_windows = sweep_windows
    prefetcher.pf_control_warmup_windows = warmup_windows


def _apply_pf_adaptive(prefetcher, cache_level, config):
    if prefetcher == NULL or not hasattr(prefetcher, 'pf_adaptive'):
        return

    control = config.get("control", {})
    adaptive = config.get("adaptive", {})
    enable = bool(adaptive.get('enabled', False))
    prefetcher.pf_adaptive = enable and cache_level in (
        'l1d', 'l2', 'l2_wrapper')
    if not prefetcher.pf_adaptive:
        return

    if not bool(control.get('enabled', False)):
        fatal(
            "PF_CONTROL_CONFIG['adaptive']['enabled'] requires "
            "PF_CONTROL_CONFIG['control']['enabled']"
        )

    weight_num, weight_den = _parse_weight_ratio(
        adaptive.get('pfbad_weight', (3, 2)))

    prefetcher.pf_adaptive_min_pct = _positive_percent_config(
        adaptive, 'min_pct', 5, "adaptive.min_pct")
    prefetcher.pf_adaptive_pct_quantum = _positive_percent_config(
        adaptive, 'pct_quantum', 10, "adaptive.pct_quantum")
    prefetcher.pf_adaptive_gradient_step = _nonnegative_int_config(
        adaptive, 'gradient_step', 10, "adaptive.gradient_step")
    prefetcher.pf_adaptive_pfbad_weight_numer = weight_num
    prefetcher.pf_adaptive_pfbad_weight_denom = weight_den
    prefetcher.pf_adaptive_dpf_min_samples = _nonnegative_int_config(
        adaptive, 'dpf_min_samples', 1, "adaptive.dpf_min_samples")
    prefetcher.pf_adaptive_dpf_deadband = _nonnegative_int_config(
        adaptive, 'dpf_deadband', 0, "adaptive.dpf_deadband")
    prefetcher.pf_adaptive_improve_margin_bps = _bps_config(
        adaptive, 'improve_margin_bps', 0, "adaptive.improve_margin_bps")
    prefetcher.pf_adaptive_history_fallback = bool(
        adaptive.get('history_fallback', True))
    prefetcher.pf_adaptive_best_topk = _positive_int_config(
        adaptive, 'best_topk', 1, "adaptive.best_topk")
    prefetcher.pf_adaptive_table_entries = _nonnegative_int_config(
        adaptive, 'table_entries', 32, "adaptive.table_entries")
    prefetcher.pf_adaptive_pfbad_entries = (
        _adaptive_pfbad_entries_for_level(cache_level, adaptive)
    )
    prefetcher.pf_adaptive_warmup_windows = _nonnegative_int_config(
        adaptive, 'warmup_windows', 1, "adaptive.warmup_windows")
    prefetcher.pf_adaptive_max_source_step = _positive_percent_config(
        adaptive, 'max_source_step', 10, "adaptive.max_source_step")


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
    prefetcher.enable_cmc = False
    prefetcher.enable_bop = True
    prefetcher.enable_cdp = True
    prefetcher.enable_despacito_stream = False
    prefetcher.bop_large = XSVirtualLargeBOP(is_sub_prefetcher=True,
                                             enable_adaptoffset=False)
    prefetcher.bop_small = XSPhysicalSmallBOP(is_sub_prefetcher=True,
                                              enable_adaptoffset=False)


def _configure_cdp(prefetcher, options):
    if prefetcher == NULL or not hasattr(prefetcher, 'cdp'):
        return

    cdp = prefetcher.cdp
    if hasattr(options, 'cdp_use_dynamic_degree'):
        cdp.use_dynamic_degree = options.cdp_use_dynamic_degree
    if hasattr(options, 'cdp_accuracy_threshold'):
        cdp.accuracy_threshold = options.cdp_accuracy_threshold
    if hasattr(options, 'cdp_use_accuracy_dependent_alignment'):
        cdp.use_accuracy_dependent_alignment = (
            options.cdp_use_accuracy_dependent_alignment)


def _configure_l2_composite(prefetcher, prefetcher_name, options):
    if options.kmh_align:
        assert prefetcher_name == 'L2CompositeWithWorkerPrefetcher'
        _configure_l2_composite_kmh_align(prefetcher)
    elif prefetcher_name == 'L2CompositeWithWorkerPrefetcher':
        _configure_l2_composite_default(prefetcher)

    _configure_cdp(prefetcher, options)

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
            prefetcher.queue_size = 128
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

    pf_control_config = _load_pf_control_config()
    _apply_pf_control(prefetcher, pf_control_config)
    _apply_pf_adaptive(prefetcher, cache_level, pf_control_config)

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
