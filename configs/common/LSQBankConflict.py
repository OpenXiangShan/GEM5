from m5.util import fatal
from m5.util.convert import toMemorySize


def set_lsq_bank_conflict_cache_params(cpu, system):
    """
    Derive LSQ bank conflict model params from DCache geometry.

    Currently LSQ needs DCache set bits to build (set+bank) keys for
    bank-conflict checks. We keep this derivation in Python to avoid
    coupling LSQ with DCache internal implementations.
    """

    cache_line_size = (
        system.cache_line_size.getValue()
        if hasattr(system.cache_line_size, "getValue")
        else int(system.cache_line_size)
    )
    assoc = int(cpu.dcache.assoc)
    size = int(toMemorySize(str(cpu.dcache.size)))
    num_sets = size // (assoc * cache_line_size)
    if num_sets <= 0 or (num_sets & (num_sets - 1)) != 0:
        fatal(
            f"Invalid dcache geometry: size={cpu.dcache.size}, assoc={assoc}, "
            f"line={cache_line_size}, num_sets={num_sets}"
        )

    cpu.DcacheSetBits = num_sets.bit_length() - 1
