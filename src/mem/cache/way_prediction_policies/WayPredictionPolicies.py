from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject
from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *

class BaseWpu(SimObject):
    type = 'BaseWpu'
    cxx_header = "mem/cache/way_prediction_policies/base_wpu.hh"
    cxx_class = "gem5::way_prediction_policy::BaseWpu"
    abstract = True

    use_virtual = Param.Bool(False, "Use virtual address for prediction and update")
    miss_train = Param.Bool(False, "Train the WPU when the cache miss")
    max_addr_bits = Param.Int(48, "Maximum virtual & physical address bits")
    assoc = Param.Int(Parent.assoc, "Cache associativity")
    size = Param.MemorySize(Parent.size, "Cache capacity in bytes")
    blk_size = Param.Int(Parent.cache_line_size, "Cache block size in bytes")
    cycle_reduction = Param.Cycles(0, "Latency reduction in cycles when way prediction is correct")

class MRUWpu(BaseWpu):
    type = 'MRUWpu'
    cxx_header = "mem/cache/way_prediction_policies/mru_wpu.hh"
    cxx_class = "gem5::way_prediction_policy::MRUWpu"

class MMRUWpu(BaseWpu):
    type = 'MMRUWpu'
    cxx_header = "mem/cache/way_prediction_policies/mmru_wpu.hh"
    cxx_class = "gem5::way_prediction_policy::MMRUWpu"
    n_tags = Param.Int(Parent.assoc, "Number of tags in each set of MMRU table")

class UTagWpu(BaseWpu):
    type = 'UTagWpu'
    cxx_header = "mem/cache/way_prediction_policies/utag_wpu.hh"
    cxx_class = "gem5::way_prediction_policy::UTagWpu"
    utag_bits = Param.Unsigned(8, "Number of hashed bits in the UTag table")

class IndexMRUWpu(BaseWpu):
    type = 'IndexMRUWpu'
    cxx_header = "mem/cache/way_prediction_policies/index_mru_wpu.hh"
    cxx_class = "gem5::way_prediction_policy::IndexMRUWpu"
    way_entries = Param.MemorySize(
        "64",
        "num of active generation table entries"
    )
    way_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.way_entries,
            size=Parent.way_entries),
        "Indexing policy of active generation table"
    )
    way_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of active generation table"
    )

class RandomWpu(BaseWpu):
    type = 'RandomWpu'
    cxx_header = "mem/cache/way_prediction_policies/random_wpu.hh"
    cxx_class = "gem5::way_prediction_policy::RandomWpu"
