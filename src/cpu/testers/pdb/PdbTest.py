# SPDX-License-Identifier: BSD-3-Clause
from m5.objects.ClockedObject import ClockedObject
from m5.objects.Prefetcher import QueuedPrefetcher
from m5.params import *
from m5.proxy import Parent


class PdbTest(ClockedObject):
    type = "PdbTest"
    cxx_class = "gem5::PdbTest"
    cxx_header = "cpu/testers/pdb/pdb_test.hh"
    system = Param.System(Parent.any, "System containing the test hierarchy")
    actions = VectorParam.String(
        [], "port,operation,address,bytes,value,delay"
    )
    port = RequestPort("Ordinary CPU/SBuffer access")
    lower_port = RequestPort(
        "Seed dirty lower data and force lower replacement"
    )
    peer_port = RequestPort("Coherent peer demand accesses")


class PdbTestPrefetcher(QueuedPrefetcher):
    type = "PdbTestPrefetcher"
    cxx_class = "gem5::PdbTestPrefetcher"
    cxx_header = "cpu/testers/pdb/pdb_test.hh"
    use_virtual_addresses = False
