from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIBridge import *
class CHI_L2(ClockedObject):
    type = 'CHI_L2'
    cxx_header = "mem/xsCHI/device/CHI_L2.hh"
    cxx_class = 'gem5::xsCHI::CHI_L2'

    cpu_side_port = ResponsePort("Port for receiving requests from upper-level caches")
    mem_side_port = RequestPort("Port for sending uncached requests downstream")

    RNBridge = Param.CHIBridge("CHI CHIBridge for L2")
    # 影子 RN bridge 列表：
    # 每个元素代表一个“独立影子注入源”，在 C++ 里会逐一接收镜像请求。
    ShadowRNBridges = VectorParam.CHIBridge([], "Optional shadow CHI bridges for traffic replay")
    # 总开关：关闭时要求所有 shadow 相关配置都为空（严格失败策略）。
    shadow_enable = Param.Bool(False, "Enable shadow L2 request mirroring")
    # 地址映射三元组（按影子索引一一对应）：
    # 映射公式：A' = dst_base + (A - src_base)，其中 A 必须位于 src/window 内。
    shadow_src_bases = VectorParam.Addr([], "Per-shadow source address window base")
    shadow_window_sizes = VectorParam.Addr([], "Per-shadow source window size")
    shadow_dst_bases = VectorParam.Addr([], "Per-shadow remap destination base")
