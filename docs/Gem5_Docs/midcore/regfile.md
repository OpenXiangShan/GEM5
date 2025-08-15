# 寄存器堆读仲裁机制 (Register File Read Arbitration)

## 概述

XS-GEM5在Scheduler中实现了寄存器堆读端口的仲裁机制，用于模拟真实处理器中读端口资源竞争和调度策略。该机制包含端口配置、优先级仲裁、regCache旁路和重试机制等核心组件。

## 核心代码逻辑

### 1. 端口配置与映射 (issue_queue.cc:210-225)

**端口配置解析：**
```cpp
// 初始化时解析每个IQ的端口配置
for (int i = 0; i < outports; i++) {
    auto oport = params.oports[i];
    for (auto rfp : oport->rp) {
        int rf_type = RF_GET_TYPEID(rfp);      // 0=整数, 1=浮点
        int rf_portid = RF_GET_PORTID(rfp);    // 端口ID  
        int rf_portPri = RF_GET_PRIORITY(rfp); // 优先级
        
        if (rf_type == RF_INTID) {
            intRfTypePortId[i].push_back(std::make_pair(RF_MAKE_TYPEPORTID(rf_type, rf_portid), rf_portPri));
        }
    }
}
```

**源操作数到端口的映射 (issue_queue.cc:520-526)：**
```cpp
// 关键：第i个源操作数使用该输出端口的第i个读端口配置
for (int i = 0; i < inst->numSrcRegs(); i++) {
    if (src.isIntReg() && intRfTypePortId[pi].size() > i) {
        rfTypePortId = intRfTypePortId[pi][i];  // pi=端口号, i=源操作数索引
        scheduler->useRegfilePort(inst, psrc, rfTypePortId.first, rfTypePortId.second);
    }
    // 如果size() <= i，则该源操作数没有读端口限制
}
```

### 2. 仲裁函数 useRegfilePort

```cpp
void Scheduler::useRegfilePort(const DynInstPtr& inst, const PhysRegIdPtr& regid, int typePortId, int pri)
```

**仲裁流程：**

1. **regCache旁路检查**
   ```cpp
   if (regCache.contains(regid->flatIndex())) {
       regCache.get(regid->flatIndex());  // LRU更新
       return;  // 直接旁路，避免端口竞争
   }
   ```

2. **端口冲突检测**
   ```cpp
   if (rfPortOccupancy[typePortId].first) {
       // 端口已被占用，进行优先级仲裁
   }
   ```

3. **优先级仲裁**
   ```cpp
   if (rfPortOccupancy[typePortId].second < pri) { // 数值越小优先级越高
       arbFailedInsts.push_back(inst);  // 当前指令仲裁失败
   } else {
       arbFailedInsts.push_back(rfPortOccupancy[typePortId].first);  // 抢占低优先级指令
       rfPortOccupancy[typePortId] = std::make_pair(inst, pri);
   }
   ```

### 3. regCache机制

**LRU缓存设计：**
```cpp
boost::compute::detail::lru_cache<uint32_t, bool> regCache; // 28项LRU缓存

// 写回时更新缓存
if (dst->isIntReg()) {
    scheduler->regCache.insert(dst->flatIndex(), {});
}
```

### 4. KunminghuScheduler端口配置

**整数寄存器读端口分布 (12个端口)：**
```python
# 优先级分配：0(最高) -> 1(中等) -> 2(最低)
intIQ0: IntRD(0,0), IntRD(1,0), IntRD(6,1), IntRD(7,1)  # ALU/MULT + BRU
intIQ1: IntRD(2,0), IntRD(3,0), IntRD(4,1), IntRD(5,1)  # ALU/MULT + BRU  
intIQ2: IntRD(4,0), IntRD(5,0), IntRD(2,1), IntRD(3,1)  # ALU + BRU/MISC
intIQ3: IntRD(6,0), IntRD(7,0), IntRD(0,1), IntRD(1,1)  # ALU + DIV

# 内存指令专用端口
load0/1/2: IntRD(8,0), IntRD(9,0), IntRD(10,0)         # LOAD专用高优先级
store0/1:  IntRD(7,2), IntRD(6,2)                       # STORE地址计算
std0/1:    IntRD(5,2)+FpRD(9,0), IntRD(3,2)+FpRD(10,0) # STORE数据
```

## 仲裁失败测试

### 测试设计思路

通过修改KunminghuScheduler配置，让所有ALU争夺相同端口，制造仲裁冲突：

```python
# 修改后的配置：强制端口竞争
intIQ0: IntRD(0,0), IntRD(1,0)  # 最高优先级
intIQ1: IntRD(0,1), IntRD(1,1)  # 中等优先级，争夺端口0,1  
intIQ2: IntRD(0,2), IntRD(1,2)  # 最低优先级，争夺端口0,1
intIQ3: IntRD(0,2), IntRD(1,2)  # 最低优先级，争夺端口0,1
```

### 测试程序关键策略

1. **x0写入策略**：所有指令写入x0，避免更新regCache，增加读端口压力
2. **大量独立寄存器**：使用32个寄存器，超过regCache容量(28)
3. **无依赖设计**：消除RAW/WAR/WAW依赖，允许真正的并行调度

### 成功案例分析

**仲裁冲突时序 (周期234)：**
```
sn:33 (intIQ0, 优先级0): add x0, gp, tp
├── p35(gp) MISS → 占用端口0 ✓
└── p36(tp) MISS → 占用端口1 ✓

sn:34 (intIQ1, 优先级1): add x0, t0, t1  
├── p37(t0) MISS → 争夺端口0 ❌ 仲裁失败！
└── p38(t1) HIT  → 旁路 ✓
```

**关键日志：**
```
[sn:33] regCache MISS for p35, use read port 0 (pri=0)
[sn:34] regCache MISS for p37, use read port 0 (pri=1) 
[sn:34] arbitration failure, typePortId 0 occupied by [sn:33]
[sn:34] arbitration failed, retry  # 下周期重试成功
```

## 关键代码位置

- `src/cpu/o3/issue_queue.cc:210-225` - 端口配置解析
- `src/cpu/o3/issue_queue.cc:520-526` - 源操作数端口映射
- `src/cpu/o3/issue_queue.cc:1159` - useRegfilePort仲裁函数
- `src/cpu/o3/issue_queue.cc:427` - regCache更新
- `configs/common/FUScheduler.py` - Scheduler配置参数

## 性能优化策略

1. **regCache旁路**：28项LRU缓存显著减少读端口压力
2. **优先级仲裁**：确保关键路径指令优先获得端口
3. **专用端口**：LOAD使用专用高优先级端口，减少竞争
4. **重试机制**：仲裁失败指令在下周期重新调度