# Unit-Stride 向量访存拆分与 LQ 占用

本文整理当前仓库里 `unit-stride` 类向量 load 的拆分路径，以及一条架构指令在 load queue 中占几项。

结论：普通 `vle*.v`（`nf=1`）在 LQ 里占 `EMUL` 项，不是按 element 拆。一条架构指令先在 decode 拆成多个 load micro-op；每个 micro-op 在 dispatch 时进 LQ 正好 1 项。

## 1. 三层“拆分”不要混在一起

| 层 | 作用 | 会不会增加 LQ 项 |
| --- | --- | --- |
| Decode macro -> micro-op | 按向量寄存器组（或 segment 的 element）拆成多个 `DynInst` | 会。LQ 项数等于 load micro-op 数 |
| IQ `VLSplit` | 向量访存发射前固定 3 cycle delay | 不会 |
| LSQ request | 单个 micro-op 跨 cacheline 时拆成多个 packet | 不会。仍挂在同一个 `DynInst` 上 |

Fetch 只把 micro-op 做成 `DynInst`；macro 本身不进 LQ。

## 2. Decode：普通 unit-stride 按寄存器组拆

`vle8/16/32/64.v` 是 macro。`nf==1` 时拆成 `emul` 个 load uop：

```cpp
const uint32_t emul = get_emul(eew, sew, vflmul, false);
const uint32_t elem_num_per_vreg = VLEN / eew;
if (nf == 1) { // sequence load
    const uint32_t num_microops = emul;
    for (int i = 0; i < num_microops; ++i) {
        vmi.rs = i * elem_num_per_vreg;
        vmi.re = (i + 1) * elem_num_per_vreg;
        vmi.microVd = VD + i;
        vmi.offset = (i * VLEN) / 8;
        // ...
        this->microops.push_back(microop);
    }
}
```

对应代码：`src/arch/riscv/isa/vector/base/vector_mem.temp.isa` 的 `VleConstructor`。

`EMUL` 定义：

```text
EMUL = max(1, EEW / SEW * LMUL)
```

EEW 等于 SEW 时，就是 `max(1, LMUL)`。

本仓库 `VLEN = 128`（`VLENB = 16`）。每个普通 unit-stride uop：

- 最多访存 16B
- 写一个目的向量寄存器 `VD + i`
- 地址为 `EA = Rs1 + i * 16`

## 3. LQ：一个 load DynInst 占一项

dispatch 时每个 `isLoad()` uop 调用一次 `insertLoad()`，LQ 只 `advance_tail()` 一次。因此：

| 指令 | LQ 项数 | 拆分粒度 |
| --- | --- | --- |
| 普通 `vle*.v`（`nf=1`） | `EMUL` | 每个向量寄存器一组连续元素 |
| `vlm.v` | 1 | 整条 mask load |
| `vlseg*`（`nf>1`） | `vlmax * nf` | 按 element 拆，每个 uop 通常 1 个 element |
| `vle*ff.v` | `EMUL + 1` | 普通 load uop 再加一个 end uop |

`vl` 是运行时值，decode 看不到。所以 **LMUL=4 即使实际 `vl` 很小，decode 仍会生成 4 个 load uop，LQ 仍占 4 项**。之后 `initiateAcc()` 可能把尾部 uop 的 `mem_size` 算成 0，但 LQ 项已经占了。

`mem_size` 计算：

```text
vend = min(vl, re)
mem_size = (rs < vend) ? (vend - rs) * EEW / 8 : 0
```

第 `i` 个普通 unit-stride uop 的范围为：

```text
rs = i * (VLEN / EEW)
re = (i + 1) * (VLEN / EEW)
```

## 4. 后面两层都不再多占 LQ

### 4.1 IQ `VLSplit`

向量访存 ready 后不直接进普通 ready queue，而是进入 `vectorReadyQ`，再分配到 `VLSplit` unit，延迟 3 cycle 后才允许发射。

`VectorUnitStrideLoad` 不阻塞 split unit；segment / stride / indexed / FOF / store 等会阻塞它所在的 unit。这只影响发射时序，不改变 LQ 项数。

### 4.2 LSQ request

单个 uop 的访存请求如果跨 cacheline，会建成 `SplitDataRequest`，对应多个 packet。这些 packet 仍属于同一个 `DynInst`，LQ 还是 1 项。

`cross16Byte` / `unitStrideAligned` 只是统计：

```text
cross16Byte = (addr % 16) + size > 16
```

向量 load 不会因为未对齐而像标量那样直接报 misalign fault。

## 5. 例子（VLEN=128，EEW=SEW）

| 指令 | LMUL | LQ 项数 | 每项最大访存 |
| --- | ---: | ---: | ---: |
| `vle64.v` | 1 | 1 | 16B |
| `vle32.v` | 1 | 1 | 16B |
| `vle32.v` | 4 | 4 | 16B |
| `vle8.v` | 8 | 8 | 16B |
| `vlm.v` | - | 1 | mask 字节数 |
| `vlseg2e32.v` | 1 | `vlmax * 2 = 8` | 通常 4B / 项 |

## 6. 一句话

一条 unit-stride 在 LQ 里占几项：普通 `vle*.v` 占 `EMUL` 项；每项对应一个向量寄存器宽度的连续块，不是每个 element 一项。
