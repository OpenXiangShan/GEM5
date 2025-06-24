# gem5 C++ 特性

### 多态

```cpp
class PCStateBase {
virtual void advance() = 0;  // 虚函数virtual
};

class RiscvISA::PCState : public PCStateBase {
void advance() override { ... }  // 重写虚函数 override
};
```

+ **<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">虚函数</font>**<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">：使用</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="background-color:rgb(248, 248, 248);">virtual</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">关键字</font>
+ **<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">虚函数表</font>**<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">：你在输出中看到的</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="background-color:rgb(248, 248, 248);">_vptr.Serializable</font>
+ **<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">动态绑定</font>**<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">：运行时确定调用哪个版本的函数</font>

```cpp
// 基类指针指向子类对象
PCStateBase* pc = new RiscvISA::PCState();
pc->advance();  // 调用 RiscvISA::PCState 的 advance
// 在运行时通过虚函数指针来动态确定是什么类
```

类继承关系

Serializable<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>PCStateBase<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>PCStateWithNext<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>SimplePCState<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>UPCState<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> -> </font>RiscvISA::PCState

其中PCStateBase 只包含_pc, _upc

PCStateWithNext 才添加 _npc

本质上通过虚函数指针来实现

在gdb 中

```cpp
-exec print *pc.get()
$4 = {<gem5::Serializable> = {_vptr.Serializable = 0x555558b6a5a0 <vtable for gem5::RiscvISA::PCState+16>, static path = {c = std::deque with 0 elements}}, _pc = 0x80000b82, _upc = 0x0}
// 只能输出基类信息，想查看npc 
-exec print *(gem5::RiscvISA::PCState*)(pc.get())
$2 = {<gem5::GenericISA::UPCState<4>> = {<gem5::GenericISA::SimplePCState<4>> = {<gem5::GenericISA::PCStateWithNext> = {<gem5::PCStateBase> = {<gem5::Serializable> = {_vptr.Serializable = 0x555558b6a5a0 <vtable for gem5::RiscvISA::PCState+16>, static path = {c = std::deque with 0 elements}}, _pc = 0x80000b82, _upc = 0x0}, _npc = 0x80000b88, _nupc = 0x1}, <No data fields>}, <No data fields>}, _compressed = 0x0, _rv32 = 0x0}

-exec print ((gem5::RiscvISA::PCState*)(pc[0].get()))->_npc
// 输出其中变量，需要多打几层括号
-exec watch ((gem5::RiscvISA::PCState*)(pc[0].get()))->_npc
// 通过watch 能找到其中的变化位置
```

通过强制类型转化，把基类转化为子类来输出，能看到_npc, _rv32 等子类才有的信息了

类型转换

```cpp
dynamic_cast<gem5::RiscvISA::PCState*>(pc.get())
在运行时进行类型检查
    
static_cast<gem5::RiscvISA::PCState*>(pc.get())
在编译时进行类型检查， 不进行运行时检查
性能好但不太安全， 没有运行时开销，程序员需要确保转换的安全性
```



npc 更新时机


不应该watch 全局的pc, 应该watch dyn_inst 内部的pc 的_npc!

watch ((gem5::RiscvISA::PCState*)instruction.data->pc)->_npc

还不能直接watch 这个，需要watch *&((gem5::RiscvISA::PCState*)instruction.data->pc)->_npc, watch 地址才行

最后发现是blt 指令在执行时候设置的npc。

```cpp
inst->execute();    // 执行这条指令
    
Fault Blt::execute(ExecContext *xc,
        Trace::InstRecord *traceData) const
        set(__parserAutoPCState, xc->pcState());	// 复制pc
                    if (Rs1 < Rs2) {
                        NPC = PC + imm;	// b82 + 6 = b88
                    } else {
                        NPC = NPC;
                    }
                ;
        __parserAutoPCState.npc(NPC);	// 设置其中npc 项
xc->pcState(__parserAutoPCState);	// 更新pc 的npc 项
执行时候把pc 更新为新的npc
```

这种不适用watchpoints 应该还挺难发现的！

### 智能指针
```cpp
std::unique_ptr<PCStateBase> next_pc(pc->clone());
```

核心就是unique_ptr, 用于自动管理动态分配的内存

+ **<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">独占所有权</font>**<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">：一个资源只能被一</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">个</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="background-color:rgb(248, 248, 248);">unique_ptr</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">拥有</font>
+ **<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">自动释放</font>**<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">：当</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="background-color:rgb(248, 248, 248);">unique_ptr</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">离开作用域时</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">自动删除对象</font>
+ **<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">不可复制</font>**<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">：只能移动（move），不能复制（copy）</font>

```cpp
    std::unique_ptr<PCStateBase> pc;
```

+ <font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">pc 是一个智能指针，指向堆上的PCState对象</font>
+ 这个PCState对象实际上是RiscvISA::PCState类型（通过多态）

pc->clone() 创建了一个深拷贝，防止影响原本pc, next_pc 接管这个对象

```cpp
   // 内存示意图
   pc     ---> [ PCState对象1 ]
   next_pc ---> [ PCState对象2 ] (对象1的副本)
```

最后mispredicted() 函数退出时候，next_pc 自动释放！

如果不用只能指针，需要手动维护它

```cpp
bool mispredicted() {
    // 手动管理内存
    PCStateBase* next_pc = pc->clone();  // 新建
    staticInst->advancePC(*next_pc);
    bool result = *next_pc != *predPC;
    delete next_pc;  // 手动释放
    return result;
}
```



shared_ptr, 默认有个引用计数，当引用时候cnt++, 删除指向它的指针或者结束作用域时候cnt--, 当cnt = 0 时候自动释放指针指向内容

```cpp
    // 创建shared_ptr
    std::shared_ptr<PCState> pc1(new PCState());
    
    // 可以复制，引用计数增加
    std::shared_ptr<PCState> pc2 = pc1;  // 引用计数=2
    // 只有当引用计数降为0时，PCState才会被删除
}  // 作用域结束，PCState被删除
```

<font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">选择建议：</font>

+ <font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">默认使用</font><font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);"> </font><font style="background-color:rgb(248, 248, 248);">unique_ptr</font>
+ <font style="color:rgb(59, 59, 59);background-color:rgb(248, 248, 248);">确实需要共享所有权时使用 </font><font style="background-color:rgb(248, 248, 248);">shared_ptr</font>

<font style="background-color:rgb(248, 248, 248);">例如// 多个流水线阶段需要访问同一个寄存器文件</font>

