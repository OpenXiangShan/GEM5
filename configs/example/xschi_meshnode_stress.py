import argparse

import m5
from m5.objects import (
    AddrRange,
    CHIPort,
    CHIStressEndpoint,
    MeshNode,
    Root,
    SrcClockDomain,
    SimpleMemory,
    System,
    SystemXBar,
    TwoMeshStressSys,
    VoltageDomain,
)


MESH_COORD_BITS = 5


def node_id(x: int, y: int, local_port: int) -> int:
    base = ((x << MESH_COORD_BITS) | y) << 3
    return base if local_port == 0 else (base | 0b100)


def build_system(args):
    system = System()
    system.clk_domain = SrcClockDomain(
        clock=args.clock,
        voltage_domain=VoltageDomain(),
    )
    system.mem_mode = "timing"

    system.membus = SystemXBar()
    system.physmem = SimpleMemory(range=AddrRange("512MB"))
    system.physmem.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    # Ports for sender <-> mesh0(local0), mesh0(east) <-> mesh1(west),
    # mesh1(local0) <-> receiver.
    system.sender_port = CHIPort(recv_buffer_size=args.port_recv_buf)
    system.mesh0_local0 = CHIPort(recv_buffer_size=args.port_recv_buf)
    system.mesh0_east = CHIPort(recv_buffer_size=args.port_recv_buf)
    system.mesh1_west = CHIPort(recv_buffer_size=args.port_recv_buf)
    system.mesh1_local0 = CHIPort(recv_buffer_size=args.port_recv_buf)
    system.receiver_port = CHIPort(recv_buffer_size=args.port_recv_buf)

    system.mesh0 = MeshNode(
        node_x=0,
        node_y=0,
        voq_depth=args.voq_depth,
        voq_depth_per_ingress=args.voq_depth_per_ingress,
        port_local0=system.mesh0_local0,
        port_east=system.mesh0_east,
    )
    system.mesh1 = MeshNode(
        node_x=1,
        node_y=0,
        voq_depth=args.voq_depth,
        voq_depth_per_ingress=args.voq_depth_per_ingress,
        port_local0=system.mesh1_local0,
        port_west=system.mesh1_west,
    )

    sender_id = node_id(0, 0, 0)
    receiver_id = node_id(1, 0, 0)

    system.sender = CHIStressEndpoint(
        networkPort=system.sender_port,
        enable_sender=True,
        total_flits=args.total_flits,
        inject_per_cycle=args.inject_per_cycle,
        src_id=sender_id,
        tgt_id=receiver_id,
        base_addr=args.base_addr,
        addr_stride=args.addr_stride,
        payload_size=args.payload_size,
        receiver_block_period=0,
        receiver_block_cycles=0,
    )

    system.receiver = CHIStressEndpoint(
        networkPort=system.receiver_port,
        enable_sender=False,
        total_flits=0,
        inject_per_cycle=1,
        src_id=receiver_id,
        tgt_id=sender_id,
        base_addr=args.base_addr,
        addr_stride=args.addr_stride,
        payload_size=args.payload_size,
        receiver_block_period=args.receiver_block_period,
        receiver_block_cycles=args.receiver_block_cycles,
    )

    system.stress_topo = TwoMeshStressSys(
        sender=system.sender,
        receiver=system.receiver,
        mesh0=system.mesh0,
        mesh1=system.mesh1,
    )

    return system


def get_argparser():
    parser = argparse.ArgumentParser(
        description="Two-MeshNode xsCHI stress: sender high-rate inject + receiver periodic block"
    )
    parser.add_argument("--clock", default="1GHz")
    parser.add_argument("--sim-ticks", type=int, default=500000)
    parser.add_argument("--total-flits", type=int, default=20000)
    parser.add_argument("--inject-per-cycle", type=int, default=8)
    parser.add_argument("--payload-size", type=int, default=64)
    parser.add_argument("--base-addr", type=int, default=0x100000)
    parser.add_argument("--addr-stride", type=int, default=64)

    parser.add_argument("--port-recv-buf", type=int, default=4)
    parser.add_argument("--voq-depth", type=int, default=2)
    parser.add_argument("--voq-depth-per-ingress", action="store_true")

    parser.add_argument("--receiver-block-period", type=int, default=32)
    parser.add_argument("--receiver-block-cycles", type=int, default=24)
    return parser


def parse_stat(stats_text: str, suffix: str) -> float:
    for line in stats_text.splitlines():
        if not line or line.startswith("-"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        if parts[0].endswith(suffix):
            try:
                return float(parts[1])
            except ValueError:
                return 0.0
    return 0.0


def main():
    args = get_argparser().parse_args()
    system = build_system(args)

    root = Root(full_system=False, system=system)
    m5.instantiate()

    event = m5.simulate(args.sim_ticks)
    print(f"Exiting @ tick {m5.curTick()} because {event.getCause()}")

    m5.stats.dump()
    m5.stats.reset()

    stats_path = f"{m5.options.outdir}/stats.txt"
    with open(stats_path, "r", encoding="utf-8") as f:
        stats_text = f.read()

    tx_sent = parse_stat(stats_text, "sender.stress.tx_sent")
    tx_fail = parse_stat(stats_text, "sender.stress.tx_send_fail")
    wakeups = parse_stat(stats_text, "sender.stress.tx_wakeup_events")
    rx_accept = parse_stat(stats_text, "receiver.stress.rx_accepted")
    rx_block = parse_stat(stats_text, "receiver.stress.rx_blocked_periodic")
    rx_mismatch = parse_stat(stats_text, "receiver.stress.rx_target_mismatch")

    print("\n[CHI Mesh Stress Summary]")
    print(f"sender.tx_sent={tx_sent:.0f}")
    print(f"sender.tx_send_fail={tx_fail:.0f}")
    print(f"sender.tx_wakeup_events={wakeups:.0f}")
    print(f"receiver.rx_accepted={rx_accept:.0f}")
    print(f"receiver.rx_blocked_periodic={rx_block:.0f}")
    print(f"receiver.rx_target_mismatch={rx_mismatch:.0f}")

    ok = True
    if tx_sent <= 0 or rx_accept <= 0:
        ok = False
        print("FAIL: no effective forwarding observed")
    if rx_block <= 0:
        ok = False
        print("FAIL: periodic receiver blocking did not trigger")
    if rx_mismatch != 0:
        ok = False
        print("FAIL: route mismatch detected at receiver")

    if ok:
        print("PASS: mesh wake-up and routing stress test passed")
        raise SystemExit(0)

    raise SystemExit(1)


if __name__ in ("__main__", "__m5_main__"):
    main()
