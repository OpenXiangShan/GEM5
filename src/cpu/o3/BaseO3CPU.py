# Copyright (c) 2016, 2019 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2005-2007 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.defines import buildEnv
from m5.params import *
from m5.proxy import *

from m5.objects.BaseCPU import BaseCPU
from m5.objects.FuncScheduler import *
#from m5.objects.O3Checker import O3Checker
from m5.objects.BranchPredictor import *
from m5.objects.ValuePredictor import *
from m5.SimObject import *

class SMTFetchPolicy(ScopedEnum):
    vals = [ 'RoundRobin', 'Branch', 'IQCount', 'LSQCount' ]

class SMTDecodePolicy(ScopedEnum):
    vals = [ 'ICount', 'DelayedICount', 'MultiPriority', 'RoundRobin' ]

class SMTFetchBlockPolicy(ScopedEnum):
    vals = [ 'BaseLine', 'BlockPolicy' ]

class SMTQueuePolicy(ScopedEnum):
    vals = [ 'Dynamic', 'Partitioned', 'Threshold', 'DynamicBorrowing' ]

class SMTLSQMode(ScopedEnum):
    vals = [ 'Independent', 'Shared' ]

class CommitPolicy(ScopedEnum):
    vals = [ 'RoundRobin', 'OldestReady' ]

class ROBWalkPolicy(ScopedEnum):
    vals = [ 'Rollback', 'Replay', 'ConstCycle', 'NaiveCpt', 'ConfidentCpt' ]

class ROBCompressPolicy(ScopedEnum):
    vals = [ 'none', 'kmhv2', 'MohBoE' ,'kmhv3' ]

class PerfRecord(ScopedEnum):
    vals = [
        # position tick
        'AtFetch', 'AtDecode', 'AtRename', 'AtDispQue', 'AtIssueQue', 'AtIssueArb', 'AtIssueReadReg',
        'AtFU', 'AtBypassVal', 'AtWriteVal', 'AtCommit',
        'Result', 'DisAsm', 'PC'
    ]

class BaseO3CPU(BaseCPU):
    type = 'BaseO3CPU'
    cxx_class = 'gem5::o3::CPU'
    cxx_header = 'cpu/o3/dyn_inst.hh'
    cxx_exports = [
        PyBindMethod("addHintDownStream"),
    ]

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._downstream_pf = []

    # Override the normal SimObject::regProbeListeners method and
    # register deferred event handlers.
    def regProbeListeners(self):
        print("Registering probe listeners for BaseO3CPU {}".format(self))
        assert len(self._downstream_pf) <= 1
        if len(self._downstream_pf):
            self.getCCObject().addHintDownStream(self._downstream_pf[0].getCCObject())
        self.getCCObject().regProbeListeners()

    def add_pf_downstream(self, other_prefetcher):
        if not isinstance(other_prefetcher, SimObject):
            raise TypeError("other_prefetcher must be a SimObject type")
        self._downstream_pf.append(other_prefetcher)

    @classmethod
    def memory_mode(cls):
        return 'timing'

    @classmethod
    def require_caches(cls):
        return True

    @classmethod
    def support_take_over(cls):
        return True

    activity = Param.Unsigned(0, "Initial count")

    cacheStorePorts = Param.Unsigned(200, "Cache Ports. "
          "Constrains stores only.")
    cacheLoadPorts = Param.Unsigned(200, "Cache Ports. "
          "Constrains loads only.")

    decodeToFetchDelay = Param.Cycles(1, "Decode to fetch delay")
    renameToFetchDelay = Param.Cycles(1 ,"Rename to fetch delay")
    iewToFetchDelay = Param.Cycles(1, "Issue/Execute/Writeback to fetch "
                                   "delay")
    commitToFetchDelay = Param.Cycles(3, "Commit to fetch delay")
    fetchWidth = Param.Unsigned(16, "Fetch width")
    fetchBufferSize = Param.Unsigned(66, "Fetch buffer size in bytes")
    enableTwoFetch = Param.Bool(False,
        "Enable two consecutive FTQ targets from one fetch buffer")
    twoFetchMaxBytes = Param.Unsigned(64,
        "Maximum fetch-buffer window covered by two-fetch")
    fetchQueueSize = Param.Unsigned(48, "Fetch queue size in micro-ops "
                                    "per-thread")

    renameToDecodeDelay = Param.Cycles(1, "Rename to decode delay")
    iewToDecodeDelay = Param.Cycles(1, "Issue/Execute/Writeback to decode "
                                    "delay")
    commitToDecodeDelay = Param.Cycles(1, "Commit to decode delay")
    fetchToDecodeDelay = Param.Cycles(3, "Fetch to decode delay")
    decodeWidth = Param.Unsigned(6, "Decode width")

    iewToRenameDelay = Param.Cycles(1, "Issue/Execute/Writeback to rename "
                                    "delay")
    commitToRenameDelay = Param.Cycles(1, "Commit to rename delay")
    decodeToRenameDelay = Param.Cycles(1, "Decode to rename delay")
    renameWidth = Param.Unsigned(6, "Rename width")

    commitToIEWDelay = Param.Cycles(1, "Commit to "
               "Issue/Execute/Writeback delay")
    renameToIEWDelay = Param.Cycles(1, "Rename to "
               "Issue/Execute/Writeback delay")

    enableDispatchStage = Param.Bool(False, "Enable the dispatch stage")
    numDQEntries = VectorParam.Unsigned([32, 16, 16], "Number of entries in the dispQue, (Int, Float/Vector, Mem)")
    dispWidth = VectorParam.Unsigned([8, 6, 6], "Each DispQue dispatch width")

    wbWidth = Param.Unsigned(20, "Writeback width")
    vectorMemCompletionDelay = Param.Cycles(0,
        "Extra delay from vector memory completion to IEW writeback")

    iewToCommitDelay = Param.Cycles(1, "Issue/Execute/Writeback to commit "
               "delay")
    renameToROBDelay = Param.Cycles(1, "Rename to reorder buffer delay")
    commitWidth = Param.Unsigned(8, "Commit width")

    squashWidth = Param.Unsigned(8, "Squash width with rollback/redo rob walk")
    ConstSquashCycle = Param.Unsigned(1, "Squash width with redo rob walk")
    robWalkPolicy = Param.ROBWalkPolicy('Replay', "Squash with a specific policy")
    numMaxRatSnapshot = Param.Unsigned(4,
        "Number of rename-map (RAT) checkpoints for NaiveCpt recovery")
    ratSnapshotDistance = Param.Unsigned(32,
        "Minimum instructions between successive RAT checkpoints")
    robWalkByDestRegs = Param.Bool(True,
        "Size the recovery walk by destination-register reclaim, "
        "not by instruction count")

    trapLatency = Param.Cycles(13, "Trap latency")
    fetchTrapLatency = Param.Cycles(1, "Fetch trap latency")

    backComSize = Param.Unsigned(10,
            "Time buffer size for backwards communication")
    forwardComSize = Param.Unsigned(10,
            "Time buffer size for forward communication")

    LQEntries = Param.Unsigned(72, "Number of load queue entries")
    SQEntries = Param.Unsigned(56, "Number of physical store queue entries")
    StoreQueueMultiple = Param.Unsigned(1,
        "Virtual-to-physical store queue capacity multiplier (power of two)")
    phySQFullCheckAtReplay = Param.Bool(True,
        "Wait for physical store queue space before starting a full-SQ replay")

    LdPipeStages = Param.Unsigned(4, "Number of stages in the load pipeline")
    StPipeStages = Param.Unsigned(5, "Number of stages in the store pipeline")

    RARQEntries = Param.Unsigned(72, "Number of RAR queue entries")
    RAWQEntries = Param.Unsigned(32, "Number of RAW queue entries")
    RARDequeuePerCycle = Param.Unsigned(4, "Maximum number of instructions to dequeue from RAR queue per cycle")
    RAWDequeuePerCycle = Param.Unsigned(4, "Maximum number of instructions to dequeue from RAW queue per cycle")
    LoadCompletionWidth = Param.Unsigned(8, "Number of loads to complete per cycle")
    StoreCompletionWidth = Param.Unsigned(4, "Number of stores to complete per cycle")

    SbufferEntries = Param.Unsigned(16, "Number of store buffer entries")
    SbufferEvictThreshold = Param.Unsigned(7, "store buffer eviction threshold")
    storeBufferInactiveThreshold = Param.Unsigned(800, "store buffer writeback timeout threshold")

    StoreWbStage = Param.Unsigned(4, "Which PipeLine Stage store instruction writeback, 4 means S4")

    LSQDepCheckShift = Param.Unsigned(0,
            "Number of places to shift addr before check")
    LSQCheckLoads = Param.Bool(True,
        "Should dependency violations be checked for "
        "loads & stores or just stores")
    store_set_clear_period = Param.Unsigned(250000,
            "Number of load/store insts before the dep predictor "
            "should be invalidated")
    LFSTSize = Param.Unsigned(32, "Last fetched store table size")
    store_set_clear_thres = Param.Unsigned(1048576,"")
    LFSTEntrySize = Param.Unsigned(4,"The number of store table inst in every entry of LFST can contain")
    SSITSize = Param.Unsigned(1024, "Store set ID table size")
    enable_storeSet_train = Param.Bool(True, "Training store set predictor")
    EnablePHASTMDP = Param.Bool(True,
        "Use PHAST memory dependence prediction instead of StoreSets")
    mdp_violation_timing = Param.String(
        "atResolve",
        "When to recover from and train a detected MDP RAW violation: "
        "atResolve or atCommit")
    phast_num_rows = Param.Unsigned(64, "PHAST rows per history table")
    phast_associativity = Param.Unsigned(4, "PHAST table associativity")
    phast_tag_bits = Param.Unsigned(16, "PHAST tag bits")
    phast_max_counter = Param.Unsigned(16, "PHAST confidence counter max")
    phast_counter_threshold = Param.Unsigned(
        1, "Minimum PHAST confidence required to issue a prediction")
    phast_counter_increment = Param.Unsigned(
        0,
        "PHAST confidence increment after a correct prediction; 0 restores max confidence")
    phast_counter_decrement = Param.Unsigned(
        1, "PHAST confidence decrement after an incorrect prediction")
    phast_selected_target_bits = Param.Unsigned(
        5, "Target-address bits included in the PHAST path hash")
    phast_history_lengths = VectorParam.Unsigned(
        [0, 2, 4, 6, 8, 12, 16, 32],
        "Branch-history lengths for PHAST path tables, shortest to longest")
    phast_second_target_max_distance = Param.Unsigned(
        0,
        "Exclusive maximum SQ distance for a PHAST second target; 0 uses half of the virtual SQ capacity")

    BankConflictCheck = Param.Bool(True, "open Bank conflict check")
    sbufferBankWriteAccurately = Param.Bool(False, "Sbuffer write to memory with bank conflict check")
    DcacheBankBytes = Param.Unsigned(
        2,
        "Dcache bank interleave granularity in bytes for LSQ bank conflict model")
    DcacheSetBits = Param.Unsigned(8, "Dcache set bits for LSQ bank conflict model")
    DcacheSetDivNum = Param.Unsigned(1, "Dcache set div num for LSQ bank conflict model (power of two)")
    EnableLdMissReplay = Param.Bool(True, "Replay Cache missed load instrution from ReplayQueue if True")
    EnablePipeNukeCheck = Param.Bool(True, "Replay load if Raw violation is detected in loadPipe if True")
    EnableReplayBasedMDP = Param.Bool(True,
        "Use replay-based mem dependency prediction (loads don't stall in IQ, "
        "but may replay in load pipe)")
    EnableMDPStrictWait = Param.Bool(False,
        "Enable StoreSet strict-wait in mem dep prediction (checkInstStrict)")

    numPhysIntRegs = Param.Unsigned(224,
            "Number of physical integer registers")
    numPhysFloatRegs = Param.Unsigned(192, "Number of physical floating point "
                                      "registers")
    numPhysVecRegs = Param.Unsigned(192, "Number of physical vector "
                                      "registers")
    numPhysVecPredRegs = Param.Unsigned(32, "Number of physical predicate "
                                      "registers")

    # most ISAs don't use condition-code regs, so default is 0
    numPhysCCRegs = Param.Unsigned(0, "Number of physical cc registers")
    numPhysRMiscRegs = Param.Unsigned(40, "Number of physical renameable misc registers")

    # rob config
    numRobs = Param.Unsigned(1, "Number of Reorder Buffers")
    RobCompressPolicy = Param.ROBCompressPolicy('kmhv2', "Reorder Buffer Compression Policy")
    numROBEntries = Param.Unsigned(160, "Number of reorder buffer entries")
    CROB_instPerGroup = Param.Unsigned(6, "Max number of inst per group")
    phyregReleaseWidth = Param.Unsigned(6, "Physical register dealloc width")

    smtNumFetchingThreads = Param.Unsigned(1, "SMT Number of Fetching Threads")
    smtNumFetchTargetThreads = Param.Unsigned(
        1, "Maximum number of distinct SMT threads starting an FTQ fetch "
           "per cycle")
    smtNumPreDispatchThreads = Param.Unsigned(
        1, "Maximum number of distinct SMT threads advanced per cycle from "
           "the fetch queue through decode, rename, dispatch, and ROB insert")
    smtFetchPolicy = Param.SMTFetchPolicy('RoundRobin', "SMT Fetch policy")
    smtLSQMode = Param.SMTLSQMode('Independent',
                                  "SMT LSQ mode: per-thread independent or shared quota")
    smtLSQPolicy    = Param.SMTQueuePolicy('Partitioned',
                                           "SMT shared LSQ allocation policy")
    smtLQThreshold = Param.Int(108, "SMT LQ Threshold Sharing Parameter")
    smtSQThreshold = Param.Int(56, "SMT SQ Threshold Sharing Parameter")

    smtRARQPolicy   = Param.SMTQueuePolicy('Dynamic',
                                           "SMT shared RARQ allocation policy")
    smtRAWQPolicy   = Param.SMTQueuePolicy('Dynamic',
                                           "SMT shared RAWQ allocation policy")
    smtIQPolicy    = Param.SMTQueuePolicy('Partitioned',
                                          "SMT IQ Sharing Policy")
    smtIQThreshold = Param.Int(100, "SMT IQ Threshold Sharing Parameter")
    smtROBPolicy   = Param.SMTQueuePolicy('Partitioned',
                                          "SMT ROB Sharing Policy")
    smtROBThreshold = Param.Int(100, "SMT ROB Threshold Sharing Parameter")
    smtCommitPolicy = Param.CommitPolicy('RoundRobin', "SMT Commit Policy")
    smtBorrowLdstqHighWater = Param.Unsigned(
        0, "Explicit SMT borrowing LSQ high-water threshold; 0 uses percentage")
    smtBorrowLdstqHighWaterPercent = Param.Percent(
        75, "SMT borrowing LSQ high-water threshold as a percentage of LQ+SQ")
    smtBorrowDonorHoldCycles = Param.Unsigned(
        8, "Cycles to keep an SMT thread marked as a ROB borrowing donor")
    smtBorrowDonorReserveEntries = Param.Unsigned(
        8, "Minimum ROB entries reserved for a borrowing donor to resume")

    smtDecodePolicy = Param.SMTDecodePolicy('MultiPriority',
        "SMT decode select policy: ICount, DelayedICount, MultiPriority, RoundRobin")
    smtFetchBlockPolicy = Param.SMTFetchBlockPolicy('BaseLine',
        "SMT fetch block policy for long-latency loads: "
        "Baseline (no blocking) or BlockPolicy (stall fetch on long-latency load)")
    smtFetchBlockThreshold = Param.Unsigned(15,
        "Number of cycles a load must wait in the LQ before it is considered "
        "long-latency and triggers fetch blocking (T15 from Tullsen & Brown's paper)")
    smtFetchDelayedSchedulerDelay = Param.Unsigned(2,
        "Number of cycles the DelayedICount Policy delayed")
    smtBorrowThrottleCycles = Param.Unsigned(
        8, "Cycles to keep a backend-stalled SMT thread throttled at fetch, 0 means disable throttle")

    smtPregPolicy = Param.SMTQueuePolicy('Dynamic',
                                         "SMT Preg (physical register) Sharing Policy")
    smtPregFixedBase = Param.Unsigned(0,
        "Fixed per-thread base quota for DynamicBorrowing (0 = numPhysRegs/activeThreads)")
    smtPregDonorReservePercent = Param.Unsigned(
        43, "Percentage (0-100) of per-thread fair share reserved for a "
            "donor thread.  donorQuota = numPhysRegs/activeThreads * pct/100")
    smtPregBackendBackpressureDonor = Param.Bool(True,
        "Also treat a thread as a Preg borrowing donor when it is stalled "
        "on ROB/dispatch-queue-bandwidth backpressure (a resource other "
        "than Preg itself); when false, only actual Preg demand this "
        "cycle drives donor status")
    smtPregBackendBackpressureDonorHoldCycles = Param.Unsigned(
        8, "Cycles to keep a backend-backpressure-triggered Preg donor "
           "marking held after the triggering condition clears")
    smtBorrowBaseReserveEntries = Param.Unsigned(
        80, "Minimum ROB entries reserved for a borrowing base to resume")
    smtLQBorrowBaseReserveEntries = Param.Unsigned(
        60, "Minimum LQ entries reserved for a borrowing base thread to resume")
    smtLQBorrowDonorReserveEntries = Param.Unsigned(
        6, "Minimum LQ entries reserved for a borrowing donor thread to resume")
    smtSQBorrowBaseReserveEntries = Param.Unsigned(
        32, "Minimum SQ entries reserved for a borrowing base thread to resume")
    smtSQBorrowDonorReserveEntries = Param.Unsigned(
        4, "Minimum SQ entries reserved for a borrowing donor thread to resume")

    branchPred = Param.BranchPredictor(DecoupledBPUWithBTB(),
                                       "Branch Predictor")
    resolveQueueSize = Param.Unsigned(16, "Number of entries in the branch resolution queue")
    needsTSO = Param.Bool(False, "Enable TSO Memory model")

    scheduler = Param.Scheduler("")

    arch_db = Param.ArchDBer(Parent.any, "Arch DB")

    store_prefetch_train = Param.Bool(True, "Training store prefetcher with store addresses")

    # value predictor
    valuePred = Param.ValuePredictor(NULL, "valuepred unit")
    enableSelectiveVPFlush = Param.Bool(False,
        "Enable selective rollback for value prediction misprediction")

    enable_loadFusion = Param.Bool(False, "Enable load fusion")

    enableMoveElimination = Param.Bool(True, "Enable register move elimination")
    enableConstantFolding = Param.Bool(False, "Enable Constant Folding (add-immediate elimination)")
    enableMovImmElimination = Param.Bool(False, "Enable MOVI elimination")

    # Trace mode parameters for trace-driven simulation
    enableTraceMode = Param.Bool(False, "Enable trace-driven simulation mode")
    traceFile = Param.String("", "Path to trace file for trace-driven simulation")
    traceFormat = Param.String("champsim", "Trace format (champsim, cbp2025)")
    enableDecoupledBPInTrace = Param.Bool(False, "Enable decoupled branch predictor in trace mode")
    traceCheckpointInterval = Param.Unsigned(64, "Checkpoint interval for trace rollback (0 disables)")
    traceBPValidation = Param.Bool(True, "Enable branch predictor validation against trace")

    # Address mapping configuration for trace mode
    traceAddrMapMode = Param.String("linear", "Address mapping mode for trace (hash|linear)")
    traceAddrBase = Param.Addr(0x80000000, "Base address for trace address mapping (within physical memory)")
    traceAddrSize = Param.Addr(0x40000000, "Size of trace address mapping region (1GB)")
    traceAddrPageAlign = Param.Bool(True, "Align trace addresses to page boundaries")

    # Trace timing PTW/TLB cost modeling (opt-in; default off)
    traceTimingPTW = Param.Bool(False,
        "In trace mode, use timing TLB/PTW with a synthetic static page table")
    tracePTReservedBytes = Param.UInt64(64 * 1024 * 1024,
        "Bytes reserved above trace-mapped region for synthetic page tables")
    tracePTLeafPageSize = Param.UInt64(4 * 1024,
        "Synthetic mapping page size for trace timing PTW (4KiB or 2MiB)")

    # Trace-driven branch predictor training and control-flow modeling
    traceTrainBranches = Param.Bool(True,
        "Enable BP training and use real branch opcodes under trace mode")

    # On a branch misprediction (predicted vs. trace ground truth), stall
    # the fetch stage for this many cycles to emulate redirect/recovery cost.
    traceMispredictPenalty = Param.Cycles(8,
        "Cycles to stall fetch on mispredict in trace mode")

    # Enable explicit wrong-path fetch/ decode/ cancel injection in decoupled frontend
    traceEnableWrongPath = Param.Bool(True,
        "Enable wrong-path injection on BP mispredict for decoupled frontend")

    # Wrong-path injection mode: use trace instructions vs. always NOPs
    # False (default): inject NOPs and keep reader position unchanged
    # True: advance reader and use trace PCs during injection; restore reader at squash
    traceWrongPathUseTraceInst = Param.Bool(False,
        "Use trace instructions for wrong-path injection (advance + checkpoint/restore); currently unimplemented")

    # Trace mode control (exec bypass reserved for future use)
    traceExecBypass = Param.Bool(False,
        "Bypass real execute for non-mem/non-ctrl ops in trace mode; only consume timing (experimental)")
