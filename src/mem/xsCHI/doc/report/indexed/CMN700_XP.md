# CMN-700 Crosspoint (XP) 实现细节技术文档

> 深入解析交叉点路由器内部架构  
> 基于 Arm Neoverse CMN-700 r3p2 Technical Reference Manual Issue 07

---

## 目录

---

## 一、XP 概述

Crosspoint（XP）是CMN-700互连架构的核心路由组件，是实现二维网格拓扑的基础构建模块。XP负责在网格中转发CHI（Coherent Hub Interface）flit，实现设备间的高带宽、低延迟通信。

### 1.1 XP的核心功能

XP作为网格路由器，主要承担以下核心功能：

- **Flit路由转发**：根据目标地址将flit路由到正确的输出端口
- **流量控制**：基于信用（Credit）机制实现端到端的流量控制
- **仲裁调度**：多个输入端口竞争同一输出端口时进行仲裁
- **错误检测**：支持flit奇偶校验和数据校验

### 1.2 端口结构

每个XP包含最多6个设备端口（P0-P5）和4个网格端口（东/西/北/南）。不同位置的XP具有不同的端口配置能力：

| XP位置 | 网格端口 | 设备端口 | 说明 |
|--------|----------|----------|------|
| 内部XP | 4（东/西/北/南） | P0/P1（最多2个） | 标准网格节点 |
| 边缘XP | 3（缺一个方向） | P0/P1/P2（最多3个） | 网格边界 |
| 角落XP | 2（缺两个方向） | P0/P1/P2/P3（最多4个） | 网格角落 |

---

## 二、CHI 通道架构

XP支持AMBA 5 CHI协议定义的四种基本通道，用于传输不同类型的flit：

| 通道 | 全称 | 功能描述 |
|------|------|----------|
| REQ | Request | 请求通道，传输读/写/原子操作请求 |
| RSP | Response | 响应通道，传输完成响应和读数据响应 |
| SNP | Snoop | 监听通道，传输缓存一致性监听请求 |
| DAT | Data | 数据通道，传输写数据和读数据 |

### 2.1 双通道配置（Dual Channel）

CMN-700支持可选的双CHI通道配置，通过以下全局参数启用：

- `POR_2XREQ_EN_PARAM`：启用双REQ通道
- `POR_2XRSP_EN_PARAM`：启用双RSP通道
- `POR_2XSNP_EN_PARAM`：启用双SNP通道
- `POR_2XDAT_EN_PARAM`：启用双DAT通道

#### 2.1.1 通道选择机制

双通道配置下，XP使用基于TargetID的查找表（LUT）选择通道。每个XP包含16个64位寄存器（`por_mxp_multi_mesh_chn_sel_0-15`），可配置最多64个目标设备的通道映射。

> **REQ通道特殊处理**：与其他通道不同，REQ通道使用SourceID而非TargetID进行通道选择。这是因为RN-F设备的TargetID可能在地址查找后被覆盖。

#### 2.1.2 通道选择模式

通过`por_mxp_device_port_ctl`寄存器，可为每个端口配置三种通道选择模式：

| 模式 | 描述 | 适用设备 |
|------|------|----------|
| 2'h0 | 基于TGTID的通道映射 | RN*/CCRA/HN*/CCHA/SNFE/RCCAL |
| 2'h1 | 基于动态信用可用性 | RNFE/RCCAL/SNFE |
| 2'h2 | 直连模式 | RNFE |

---

## 三、XY 路由算法

CMN-700默认采用XY维度序路由算法，确保无死锁路由。该算法首先沿X维度路由，到达目标X坐标后再沿Y维度路由。

### 3.1 默认XY路由规则

XP根据当前位置与目标位置的坐标比较决定路由方向：

| 条件 | 路由方向 | 说明 |
|------|----------|------|
| 目标XID > 当前XID | 向东（East） | 沿X正方向路由 |
| 目标XID < 当前XID | 向西（West） | 沿X负方向路由 |
| 目标YID > 当前YID | 向北（North） | 沿Y正方向路由 |
| 目标YID < 当前YID | 向南（South） | 沿Y负方向路由 |
| XID和YID都匹配 | 下载到设备 | 到达目标XP |

### 3.2 非XY路由（Non-XY Routing）

CMN-700支持配置最多16个源-目标XP对使用非XY路由，以优化流量分布、减少热点。通过`por_mxp_xy_override_sel_0-7`寄存器配置。

#### 3.2.1 非XY路由寄存器格式

| 位域 | 描述 |
|------|------|
| [63] VALID | 该条目有效 |
| [62:48] SRCID | 源XP ID（11位） |
| [46:36] TGTID | 目标XP ID（11位） |
| [34] CAL_TGT_PRESENT | CAL目标存在指示 |
| [33] YX_TURN_ENABLE | 允许YX转向（先Y后X） |
| [32] XY_OVERRIDE_ENABLE | 启用XY路由覆盖 |

#### 3.2.2 死锁避免规则

启用非XY路由时必须遵守以下死锁避免规则：

- 若XP(xi,yi)允许N→W转向，则所有XP(xj,yj)（xj<xi且yj<yi）禁止S→E转向
- 若XP(xi,yi)允许S→E转向，则所有XP(xj,yj)（xj>xi且yj>yi）禁止N→W转向
- 若XP(xi,yi)允许N→E转向，则所有XP(xj,yj)（xj>xi且yj<yi）禁止S→W转向
- 若XP(xi,yi)允许S→W转向，则所有XP(xj,yj)（xj<xi且yj>yi）禁止N→E转向

---

## 四、流量控制与缓冲区

### 4.1 信用机制（Credit-Based Flow Control）

CMN-700采用端到端信用机制进行流量控制。发送方必须获得信用后才能发送flit，接收方处理完flit后返还信用。

#### 4.1.1 信用往返延迟

信用往返延迟（Credit Roundtrip Latency）决定所需的最小信用数：

| 延迟类型 | 描述 | RN-F接口典型值 |
|----------|------|----------------|
| UpCrdLatInt | XP内部上传信用延迟 | 1周期（所有通道） |
| UpCrdLatExt | XP外部上传信用延迟 | 取决于设备实现 |
| DnCrdLatInt | XP内部下载信用延迟 | 2周期（所有通道） |
| DnCrdLatExt | XP外部下载信用延迟 | 取决于设备实现 |

### 4.2 缓冲区配置

XP支持多种可配置的缓冲区参数：

| 参数 | 范围 | 描述 |
|------|------|------|
| RXBUF_NUM_ENTRIES | 2-4 | 接收缓冲区深度（每个端口） |
| MCS_RXBUF_NUM_ENTRIES | 2-4 | MCS接收缓冲区深度 |
| num_mcs_east/north | 0-4 | 东/北端口MCS数量 |
| num_dcs_p#_d# | 0-4 | 设备端口DCS数量 |
| num_ccs_p# | 0-2 | CAL端口CCS数量 |

### 4.3 信用切片（Credited Slices）

为优化时序，XP之间可插入多种信用切片：

- **MCS（Mesh Credited Slice）**：XP之间的同步切片，每个增加1周期延迟
- **AMCS（Asynchronous MCS）**：异步时钟域跨越切片
- **DCS（Device Credited Slice）**：设备与XP之间的切片
- **CCS（CAL Credited Slice）**：CAL与XP之间的切片

---

## 五、寄存器架构

每个XP拥有64KB的配置寄存器空间，包含发现信息、连接信息、控制寄存器和状态寄存器。

### 5.1 发现寄存器

| 寄存器 | 偏移 | 描述 |
|--------|------|------|
| por_mxp_node_info | 0x0 | 节点类型、逻辑ID、XY坐标、设备端口数 |
| por_mxp_child_info | 0x80 | 子节点数量和子指针偏移 |
| por_mxp_child_pointer_0-31 | 0x100+ | 最多32个子节点的配置地址 |

### 5.2 端口信息寄存器

`por_mxp_p0-5_info`寄存器提供端口设备的详细信息：

| 位域 | 描述 |
|------|------|
| num_dev_p# | 连接设备数量（0-4） |
| rxbuf_num_entries_p# | 输入缓冲区数量（2-4） |
| datacheck_en_p# | 数据校验使能 |
| poison_en_p# | 毒化使能 |
| dsu_num_p# | DSU AXU接口数量（0-4） |
| dmc_num_p# | DMC AXU接口数量（0-4） |
| a4s_num_p# | AXI4-Stream接口数量（0-4） |

### 5.3 QoS控制寄存器

XP支持每个端口的QoS调节功能：

| 寄存器 | 功能 |
|--------|------|
| por_mxp_p0-5_qos_control | QoS调节使能、模式选择、覆盖值 |
| por_mxp_p0-5_qos_lat_tgt | 目标延迟/周期（12位） |
| por_mxp_p0-5_qos_lat_scale | 缩放因子（2^-3到2^-10） |
| por_mxp_p0-5_qos_lat_range | 最小/最大QoS值 |

---

## 六、错误处理

### 6.1 错误检测机制

XP支持以下错误检测机制：

- **Flit奇偶校验**：在设备端口上传时生成，下载时检查
- **数据校验（Data Check）**：字节级奇偶校验，用于DAT通道

### 6.2 RAS寄存器

| 寄存器 | 类型 | 功能 |
|--------|------|------|
| por_mxp_errfr | RO | 错误特性寄存器（ED/DE/FI/UI/CFI） |
| por_mxp_errctlr | RW | 错误控制寄存器（使能各类中断） |
| por_mxp_errstatus | W1C | 错误状态（AV/V/UE/OF/MV/CE/DE） |
| por_mxp_errmisc | RW | 错误杂项信息（SRCID/TGTID/OPCODE） |

---

## 七、调试与性能监控

### 7.1 DTM（Debug and Trace Monitor）

每个XP可配置一个或多个DTM，用于调试跟踪和性能监控。当`MXP_MULTIPLE_DTM_EN=1`时，DTM按端口对复制：

- **DTM0**：支持P0和P1端口
- **DTM1**：支持P2和P3端口

### 7.2 PMU事件

XP PMU支持以下类型的事件计数：

| 事件类型 | 描述 |
|----------|------|
| TX flit valid | flit成功传输 |
| TX flit stall | flit传输因等待信用而停滞 |
| Partial DAT flit | 128位DAT flit未能合并为256位 |

---

## 八、配置示例

### 8.1 4×4网格配置

典型的4×4网格配置中，XP的坐标和端口能力如下：

| XP坐标 | 类型 | 网格端口 | 设备端口 |
|--------|------|----------|----------|
| (0,0)-(0,3) | 左边缘 | 东/北/南 | P0/P1/P2 |
| (3,0)-(3,3) | 右边缘 | 西/北/南 | P0/P1/P2 |
| (1,0)-(2,0) | 下边缘 | 东/西/北 | P0/P1/P2 |
| (1,3)-(2,3) | 上边缘 | 东/西/南 | P0/P1/P2 |
| (0,0) | 左下角 | 东/北 | P0/P1/P2/P3 |
| (3,3) | 右上角 | 西/南 | P0/P1/P2/P3 |
| (1,1)-(2,2) | 内部 | 东/西/北/南 | P0/P1 |

### 8.2 双通道配置示例

启用双DAT/RSP通道时，默认通道分配规则：

- `MESH_2X_DEF_SEL=0`：目标XP的XID为偶数使用通道0，奇数使用通道1
- `MESH_2X_DEF_SEL=1`：目标XP的YID为偶数使用通道0，奇数使用通道1

### 8.3 非XY路由配置示例

假设需要优化从NodeID 40（XP(1,1)）到NodeID 124（XP(3,3)）的流量路径，可配置XY覆盖使flit先向北再向东，避免(3,1)到(3,3)的拥塞。

---

## 附录：XP寄存器汇总

### 发现和连接寄存器

| 寄存器名称 | 偏移地址 | 类型 | 描述 |
|------------|----------|------|------|
| por_mxp_node_info | 0x0 | RO | 节点基本信息 |
| por_mxp_device_port_connect_info_p0-5 | 0x8+ | RO | 设备端口连接信息 |
| por_mxp_mesh_port_connect_info_east | 0x38 | RO | 东端口连接信息 |
| por_mxp_mesh_port_connect_info_north | 0x40 | RO | 北端口连接信息 |
| por_mxp_device_port_connect_ldid_info_p0-5 | 0x48+ | RO | 设备LDID信息 |
| por_mxp_child_info | 0x80 | RO | 子节点信息 |
| por_mxp_child_pointer_0-31 | 0x100+ | RO | 子节点指针 |

### 端口信息和控制寄存器

| 寄存器名称 | 偏移地址 | 类型 | 描述 |
|------------|----------|------|------|
| por_mxp_p0-5_info | 0x900+ | RO | 端口设备信息 |
| por_mxp_p0-5_info_1 | 0x908+ | RO | 端口通道复制信息 |
| por_mxp_aux_ctl | 0xA00 | RW | 辅助控制（时钟门控） |
| por_mxp_device_port_ctl | 0xA08 | RW | 设备端口控制 |
| por_mxp_p0-5_mpam_override | 0xA10+ | RW | MPAM覆盖 |
| por_mxp_p0-5_ldid_override | 0xA40+ | RW | LDID覆盖 |

### QoS寄存器

| 寄存器名称 | 偏移地址 | 类型 | 描述 |
|------------|----------|------|------|
| por_mxp_p0-5_qos_control | 0xA80+ | RW | QoS控制 |
| por_mxp_p0-5_qos_lat_tgt | 0xA88+ | RW | 目标延迟 |
| por_mxp_p0-5_qos_lat_scale | 0xA90+ | RW | 延迟缩放 |
| por_mxp_p0-5_qos_lat_range | 0xA98+ | RW | QoS范围 |

### 多通道和路由寄存器

| 寄存器名称 | 偏移地址 | 类型 | 描述 |
|------------|----------|------|------|
| por_mxp_multi_mesh_chn_sel_0-15 | 0xC00+ | RW | 多通道选择 |
| por_mxp_multi_mesh_chn_ctrl | 0xC80 | RW | 多通道控制 |
| por_mxp_xy_override_sel_0-7 | 0xC90+ | RW | XY路由覆盖 |

### RAS寄存器

| 寄存器名称 | 偏移地址 | 类型 | 描述 |
|------------|----------|------|------|
| por_mxp_errfr | 0x3000 | RO | 错误特性 |
| por_mxp_errctlr | 0x3008 | RW | 错误控制 |
| por_mxp_errstatus | 0x3010 | W1C | 错误状态 |
| por_mxp_errmisc | 0x3028 | RW | 错误杂项 |
| por_mxp_errfr_NS | 0x3100 | RO | 非安全错误特性 |
| por_mxp_errctlr_NS | 0x3108 | RW | 非安全错误控制 |
| por_mxp_errstatus_NS | 0x3110 | W1C | 非安全错误状态 |
| por_mxp_errmisc_NS | 0x3128 | RW | 非安全错误杂项 |

### 一致性域控制寄存器

| 寄存器名称 | 偏移地址 | 类型 | 描述 |
|------------|----------|------|------|
| por_mxp_p0-5_syscoreq_ctl | 0x1C00+ | RW | 监听/DVM域请求控制 |
| por_mxp_p0-5_syscoack_status | 0x1C08+ | RO | 监听/DVM域状态 |

### PMU寄存器

| 寄存器名称 | 偏移地址 | 类型 | 描述 |
|------------|----------|------|------|
| por_mxp_pmu_event_sel | 0x2000 | RW | PMU事件选择 |

---

*文档版本：基于 CMN-700 r3p2 Issue 07*  
*最后更新：2025年*
