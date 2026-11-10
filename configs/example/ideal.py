"""Initial single-core resource and frontend upper-bound preset.

Use the normal KMHV3 construction path and apply this preset before --param.
This retains real branch prediction, load execution and squash recovery.
"""

from m5.objects import IssuePort, IssueQue, ReadPort, XSDRRIPRP
from m5.util import fatal

from kmhv3 import hybrid_policy_requested, main


def configureIdealArgs(args):
    """Validate the supported topology before allocating the system."""
    if (args.num_cpus != 1 or args.smt or args.enable_spmt or args.ruby or
            args.classic_l2 or args.no_l3cache or args.enable_trace_mode or
            args.xiangshan_ecore or
            args.l2_slices != 4 or args.btb_tage_upper_bound or
            getattr(args, "standalone_sc", False) or
            hybrid_policy_requested(args)):
        fatal("ideal.py requires a single-core, single-thread KMHV3 system "
              "with four wrapper L2 slices, L3 and the normal BTB predictor")
    args.l1d_enable_lldp = True
    args.enable_value_prediction = True


def setIdealParams(args, system):
    """Apply resource and frontend defaults before user --param overrides."""
    for cpu in system.cpu:
        # Pipeline widths do not impose a global instruction issue cap.
        cpu.decodeWidth = 10
        cpu.renameWidth = 10
        cpu.commitWidth = 10
        cpu.phyregReleaseWidth = 10
        cpu.iewToFetchDelay = 2
        cpu.commitToFetchDelay = 1
        cpu.squashWidth = 32
        cpu.EnablePHASTMDP = True

        cpu.numROBEntries = 640
        cpu.numPhysIntRegs = 512
        cpu.numPhysFloatRegs = 384
        cpu.LQEntries = 256
        cpu.SQEntries = 128
        cpu.RARQEntries = 192
        cpu.RAWQEntries = 128
        cpu.mmu.dtb.size = 512

        # Keep the existing ports and IQ indices; append only the fourth load.
        for iq in cpu.scheduler.IQs:
            name = str(iq.name)
            if name.startswith("intIQ"):
                iq.deferNewEnqueueSelection = False
            if name.startswith("fpIQ"):
                iq.size = 24
        template = next(iq for iq in cpu.scheduler.IQs
                        if str(iq.name) == "ld2")
        cpu.scheduler.fourthLoadIQ = IssueQue(
            name="ld3", inports=template.inports, size=template.size,
            scheduleToExecDelay=template.scheduleToExecDelay,
            deferNewEnqueueSelection=template.deferNewEnqueueSelection,
            oports=[IssuePort(fu=[ReadPort()])])
        cpu.scheduler.IQs = (list(cpu.scheduler.IQs) +
                             [cpu.scheduler.fourthLoadIQ])
        for channel in cpu.scheduler.specWakeupNetwork:
            if "ld2" in channel.srcs:
                channel.srcs = list(channel.srcs) + ["ld3"]
            if "ld2" in channel.dsts:
                channel.dsts = list(channel.dsts) + ["ld3"]

        # Functional bytes and serial double prediction retain real mistakes.
        cpu.idealInstructionFetch = True
        cpu.enableTwoFetch = True
        cpu.branchPred.idealPredictionsPerCycle = 2
        cpu.branchPred.idealNoOverrideBubbles = True
        cpu.branchPred.tage.tableSizes = [8192] * 8
        cpu.branchPred.tage.numWays = [8] * 8
        cpu.branchPred.mgsc.enableLTable = True
        cpu.branchPred.mgsc.enablePCThreshold = True

    l2 = system.l2_wrappers[0]
    l2.cache_size = "2MiB"
    l2.prefetcher.enable_cdp = False
    l2.data_sram_banks = 4
    l2.dir_sram_banks = 4
    for cache_slice in l2.slices:
        cache_slice.inner_cache.size = "512KiB"
        cache_slice.inner_cache.replacement_policy = XSDRRIPRP(
            mode=2, num_sets=1024)
    system.l3.size = "64MiB"
    system.tol3bus.snoop_filter.max_capacity = "64MiB"


if __name__ == "__m5_main__":
    main(configure_args=configureIdealArgs, configure_system=setIdealParams)
