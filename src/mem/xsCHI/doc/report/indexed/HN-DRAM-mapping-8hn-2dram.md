# xsCHI 8 HN / 2 DRAM 映射说明

## 1. 文档目的

本文档用于简要说明在 `L2L3DramSys_5x3` 拓扑下：

- 当 `HN = 8` 时，`RN -> HN` 如何选路
- 当 `DRAM = 2` 时，`HN -> DRAM` 如何选路
- 为什么 `8` 个 `HN` 与 `2` 个 `DRAM` 可以自然对齐

适用背景：

| 项目 | 值 |
|---|---|
| Topology | `L2L3DramSys_5x3` |
| HN 数量 | `8` |
| DRAM 数量 | `2` |

相关实现位置：

| 类型 | 文件 |
|---|---|
| RN/HN 哈希规则 | `src/mem/xsCHI/base/Network/SystemAddressMap.hh` |
| 5x3 拓扑绑定 | `src/mem/xsCHI/TopoSys/L2L3DramSys5x3.cc` |
| Python 侧 DRAM interleaved range 构造 | `configs/common/CacheConfig.py` |

## 2. 两级映射的基本关系

当前 xsCHI 使用两级地址映射：

| 级别 | 作用 | 目标集合 |
|---|---|---|
| `RN SAM` | 决定请求先落到哪个 `HN` | `8` 个 `HN` |
| `HN SAM` | 决定该 `HN` 再把请求发往哪个 `DRAM` | `2` 个 `DRAM` |

这不是把 `8` 个 `HN` “压缩”成 `2` 个 `DRAM`，而是：

1. 同一个物理地址先映射到一个 `HN`
2. 该 `HN` 再用同一个物理地址映射到一个 `DRAM`

关键点在于：**这两级哈希使用相同风格的 XOR 规则，只是目标数量不同。**

## 3. 8 个 HN 的选路规则

`SystemAddressMapRN` 的规则见 `SystemAddressMap.hh`。

当目标数量为 `8` 时：

| 项目 | 值 |
|---|---|
| `target_count` | `8` |
| `SelectBits` | `log2(8) = 3` |

因此会得到 3 个选择位：

```text
h0 = XOR(addr[6],  addr[9],  addr[12], ...)
h1 = XOR(addr[7],  addr[10], addr[13], ...)
h2 = XOR(addr[8],  addr[11], addr[14], ...)
```

最终：

```text
HN_index = h0 + 2*h1 + 4*h2
```

因此一个地址会被映射到 `0~7` 中的某一个 `HN index`。

## 4. 2 个 DRAM 的选路规则

`SystemAddressMapHN` 与 `SystemAddressMapRN` 使用同构逻辑。

当目标数量为 `2` 时：

| 项目 | 值 |
|---|---|
| `target_count` | `2` |
| `SelectBits` | `log2(2) = 1` |

因此只会生成 1 个选择位：

```text
d = XOR(addr[6], addr[7], addr[8], ..., addr[51])
```

最终：

```text
DRAM_index = d
```

也就是目标只会是：

| `DRAM_index` | 目标 |
|---|---|
| `0` | `DRAM[0]` |
| `1` | `DRAM[1]` |

## 5. 8 HN 与 2 DRAM 为什么能对齐

这是最关键的地方。

对 `8 HN` 而言，地址位被分成了 3 组：

- `h0` 使用 bit `6, 9, 12, ...`
- `h1` 使用 bit `7, 10, 13, ...`
- `h2` 使用 bit `8, 11, 14, ...`

而对 `2 DRAM` 而言：

- `d` 使用 bit `6..51` 的整体 XOR

因此有：

```text
d = h0 ^ h1 ^ h2
```

这意味着：

- `HN_index` 一旦确定
- `DRAM_index` 也随之确定

换句话说，`2` 个 `DRAM` 不是去适配 `8` 个完全独立的目标，而是通过地址位奇偶关系，把 `8` 个 `HN index` 自动分成两组。

## 6. 8 HN / 2 DRAM 的实际对应关系

由 `d = h0 ^ h1 ^ h2` 可直接推出：

| HN index | HN bits `(h2 h1 h0)` | DRAM index |
|---|---|---|
| `0` | `000` | `0` |
| `1` | `001` | `1` |
| `2` | `010` | `1` |
| `3` | `011` | `0` |
| `4` | `100` | `1` |
| `5` | `101` | `0` |
| `6` | `110` | `0` |
| `7` | `111` | `1` |

因此可以把 `8` 个 `HN` 看成被自动划分为两组：

| DRAM index | 对应 HN index |
|---|---|
| `0` | `0, 3, 5, 6` |
| `1` | `1, 2, 4, 7` |

## 7. 与 attach point 的关系

`HN index` 和 `DRAM index` 都是**按命令行传入顺序**建立的，不是按 mesh 编号自动排序。

例如：

```text
--chi-hn-attach-points=A,B,C,D,E,F,G,H
```

则：

| HN index | attach point |
|---|---|
| `0` | `A` |
| `1` | `B` |
| `2` | `C` |
| `...` | `...` |
| `7` | `H` |

同理：

```text
--chi-dram-attach-points=X,Y
```

则：

| DRAM index | attach point |
|---|---|
| `0` | `X` |
| `1` | `Y` |

因此“某个 HN 最终打到哪个 DRAM”，本质上是：

```text
地址 -> HN_index -> DRAM_index -> attach point
```

## 8. 一个重要澄清：HN 侧会再次通过 PA 计算

实现上，`SystemAddressMapHN` **不是直接拿 `HN` 的 3bit 哈希值去选 `DRAM`**，而是会再次读取同一个物理地址 `PA`，重新执行一次 `getTargetID(addr)`。

也就是说，运行时真实路径是：

1. `RN` 侧用 `PA` 计算 `HN target`
2. `HN` 侧再用**同一个 `PA`** 计算 `DRAM target`

因此：

| 层面 | 结论 |
|---|---|
| 实现路径 | `HN -> DRAM` 仍然是 `PA -> DRAM`，不是 `HN_index -> DRAM` |
| 数学推导 | 在 `8 HN / 2 DRAM` 下，`DRAM` 的选择位可以由 `HN` 的 3 个选择位推出 |

换句话说：

- “`HN` 的 3bit 结果决定了 `DRAM` 去向”是**分析上的等价关系**
- “`SystemAddressMapHN` 直接使用 `HN_index` 选 `DRAM`”则不是实现行为

之所以可以用 `HN` 的 3bit 结果来解释，是因为在本场景下：

```text
d = h0 ^ h1 ^ h2
```

即：

- `HN` 的 3 个选择位一旦确定
- `DRAM` 的 1 个选择位也就能被严格推出

但实现仍然是重新基于 `PA` 计算，而不是复用 `HN_index`。

## 9. 为什么需要 interleaved DRAM ranges

当 `HN` 按上述 XOR 规则把地址发往 `DRAM[0]` 或 `DRAM[1]` 时，`DDRWrapper.range` 也必须遵守同样的地址归属规则。

否则如果简单把内存连续切成两半：

- `DRAM[0] = lower half`
- `DRAM[1] = upper half`

就会出现：

- HN 哈希选中了 `DRAM[1]`
- 但该地址实际上落在 `DRAM[0]` 的连续区间

这会导致路由和地址归属不一致。

因此当前实现改为：

- 按 `SystemAddressMapHN` 同样的 XOR masks
- 为每个 `DRAM` 生成对应的 interleaved `AddrRange`

这样才能保证：

```text
HN 选中的 DRAM == 该地址真正归属的 DRAM
```

## 10. 结论

可将 `8 HN / 2 DRAM` 的关系总结为：

| 结论 | 说明 |
|---|---|
| 不是人工绑定 | 不是手工规定 “哪些 HN 归哪个 DRAM” |
| 是地址驱动的两级哈希 | 同一地址先选 `HN`，再选 `DRAM` |
| 8 HN 与 2 DRAM 天然可对齐 | 因为 `DRAM` 的 1-bit 选择值正好等于 `HN` 的 3-bit 选择值的 XOR |
| HN 侧仍会再次看 PA | 这是实现行为；“由 HN 3bit 推出 DRAM”只是数学等价解释 |
| interleaved DRAM range 是必须的 | 否则地址归属与 HN 选路会冲突 |

一句话概括：

> `2` 个 `DRAM` 不是去“适配” `8` 个 `HN`，而是与 `8` 个 `HN` 共享同一套地址位逻辑，只是观察这个逻辑的粒度不同。
