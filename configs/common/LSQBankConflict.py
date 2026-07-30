from m5.util import fatal
from m5.util.convert import toMemorySize


def _param_to_int(value):
    return int(value.getValue()) if hasattr(value, "getValue") else int(value)


def set_lsq_bank_conflict_cache_params(cpu, system):
    """
    Derive LSQ bank conflict model params from DCache geometry.

    The LSQ needs DCache set bits to build (set+bank) keys for bank-conflict
    checks. Its optional Hash Tag Array also mirrors DCache associativity and
    enumerates the extra virtual-indexed sets that can alias within its VIPT
    page size. Keep this derivation in Python to avoid coupling LSQ with
    DCache internal implementations.
    """

    cache_line_size = _param_to_int(system.cache_line_size)
    assoc = _param_to_int(cpu.dcache.assoc)
    size = int(toMemorySize(str(cpu.dcache.size)))
    if cache_line_size <= 0 or assoc <= 0:
        fatal(
            f"Invalid dcache geometry: size={cpu.dcache.size}, assoc={assoc}, "
            f"line={cache_line_size}"
        )

    set_capacity = assoc * cache_line_size
    num_sets = size // set_capacity
    if (
        (cache_line_size & (cache_line_size - 1)) != 0
        or size % set_capacity != 0
        or num_sets <= 0
        or (num_sets & (num_sets - 1)) != 0
    ):
        fatal(
            f"Invalid dcache geometry: size={cpu.dcache.size}, assoc={assoc}, "
            f"line={cache_line_size}, num_sets={num_sets}"
        )

    dcache_set_bits = num_sets.bit_length() - 1
    dcache_line_bits = cache_line_size.bit_length() - 1
    tags = cpu.dcache.tags
    page_size_sources = []
    if hasattr(tags, "page_size"):
        page_size_sources.append(("tags", _param_to_int(tags.page_size)))

    indexing_policy = getattr(tags, "indexing_policy", None)
    if indexing_policy is not None and hasattr(indexing_policy, "page_size"):
        page_size_sources.append(
            ("indexing_policy", _param_to_int(indexing_policy.page_size))
        )

    if page_size_sources:
        page_sizes = {page_size for _, page_size in page_size_sources}
        if len(page_sizes) != 1:
            page_size_desc = ", ".join(
                f"{source}={page_size}"
                for source, page_size in page_size_sources
            )
            fatal(f"Inconsistent dcache VIPT page sizes: {page_size_desc}")

        page_size = page_size_sources[0][1]
        if page_size <= 0 or (page_size & (page_size - 1)) != 0:
            fatal(
                "Dcache VIPT page size must be a positive power of two: "
                f"{page_size}"
            )
        dcache_alias_bits = max(
            0, dcache_line_bits + dcache_set_bits - (page_size.bit_length() - 1)
        )
    else:
        # Non-VIPT caches have no synonym sets to enumerate.
        dcache_alias_bits = 0

    cpu.DcacheSetBits = dcache_set_bits
    cpu.DcacheAssoc = assoc
    cpu.DcacheAliasBits = dcache_alias_bits
