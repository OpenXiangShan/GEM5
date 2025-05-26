# AI 辅助编写单元测试

### 动机
用cursor也比较久了，经常在网上看到用cursor来辅助写代码，写demo，写单元测试，重构代码的宣传，就像自己做实验试试。

网上也有一些教程

[https://cursor101.com/zh](https://cursor101.com/zh)

先是自己用cursor写了下网页，写了个五子棋，都还是挺快的。

然后想用cursor写个cpu 模拟器，之前对gem5 的事件驱动一直掌握的不深，想写个demo；

大概花了1天，大概写了5000行代码，都非常简单，但基本实现了5级顺序流水线，带memory, 能执行非常大约10条的RISCV 指令（所有译码功能基本都正确，还挺离谱的），最后添加完bypass 逻辑后就暂时没往下做了。分析下代码：大致写了1000行的src, 1000行的include, 3000行的tests

发现AI 或者好的软件工程项目都是同时编写代码、编写文档、编写单元测试。这三个同时推进，就能更好的维护大型项目。所以接下来想在gem5 中实践下。

### 为何写单元测试
背景：

1. 之前前端代码主要是勾凌睿大佬写的，注释较少，不太敢修改代码
2. 之前自己写小型benchmark来测试某个模块，发现要么benchmark 写的不太对（手写汇编，打commitTrace验证），或者写对了，但是其他模块可能影响了他，并没有走到我关心的那个模块中，并且打印log，调试耗时非常长
3. 各个模块耦合性强，AI 也不太理解各个模块是如何交互的



目标

1. 帮助我更好的理解某个模块的代码逻辑和细节，尽可能遍历到代码的每个if else 逻辑
2. 补全代码注释，方便其他人学习和修改
3. 为之后重构代码、添加新的逻辑做准备
4. 划清模块边界，理解清楚各个模块相互接口，方便AI 之后辅助编程



原则：我们人类是如何理解代码，添加单元测试的？

<font style="color:#DF2A3F;">压缩产生智能</font>

我们自己看一遍代码，也不会背下来所有代码，而是用抽象的记忆或者文档总结它，搞明白每个接口含义就好，之后会用它，好的习惯就是把他们总结为接口文档、设计文档。理解、总结的过程就是一种信息压缩。压缩可能产生歧义，之后再重复这个流程来不断修正它。

AI 有个最严重的问题：无法长时间记忆，一段代码读完后，再开一个窗口，记忆都消失了，所以必须要维护这个压缩后的记忆，持久化存储在项目中，用文档or注释的形式，防止AI 产生幻觉！



注意：AI 只是辅助编程，类似自动驾驶L2级别，你自己一定要理解怎么做，大致清楚整个过程，不要期待AI 比你强太多，什么都交给他写，这样一定会出错，他生成的代码一定要自己检查下是否合理！

### 如何添加单元测试
<font style="color:#DF2A3F;">注意：目前所有的单元测试添加已经合入主线，可以参考</font>

1. 首先让AI 为这个模块添加英文注释，并维护一个接口文档。注意：这可能有错误，之后需要修正，也需要自己大致阅读清楚代码，来修正注释。
2. 复制这个模块到test目录下，修改这个模块为mock 类
    1. 暂时去除继承关系，去除和gem5 相关的各种依赖，修改include文件尽可能少
    2. 把private 方法、变量修改为public, 方便白盒测试
    3. include "cpu/pred/btb/test/test_dprintf.hh"  会把gem5 默认的DPRINTF 宏定义覆盖定义为printf, 防止依赖gem5自带的print
    4. 删除掉可能存在的archdb 数据库统计功能
    5. 把gem5 stats改成普通的uint, 防止依赖gem5自带的统计函数

```cpp
    struct TageStats {
        uint64_t predNoHitUseBim;

    struct TageStats : public statistics::Group {
        statistics::Scalar predNoHitUseBim;
```

3. 这样有如下优缺点：
    1. 缺点：我需要同时维护两套代码，一个地方修改后，另一个地方还要手动修改，还是挺麻烦的，暂时还没想到更好的解决方法，用更好的模版or继承能解决，等之后做吧。
    2. 优点：先随便在mock 类修改，测试完备后再迁移到原本类中
4. 自己需要理解上下文，也就是这个类中各个函数是如何调用的，有什么先后关系，总结到文档中，例如TAGE: 外围需要输入stream, 需要维护全局历史，然后按循序调用预测、更新、冲刷函数

```markdown
1. Prediction Phase:
```cpp
// Setup BTB entries and history
std::vector<BTBEntry> btbEntries;
boost::dynamic_bitset<> history(64, 0);

// Make prediction to generate meta data
tage->putPCHistory(startPC, history, stagePreds);

// update (folded) histories for tage
tage->specUpdateHist(s0History, finalPred);
tage->getPredictionMeta();

// shift history
histShiftIn(shamt, taken, s0History);
//     history <<= shamt;
//     history[0] = taken;
tage->checkFoldedHist(s0History, "speculative update");

```

2. Update Phase:
```cpp
// Setup update stream
FetchStream stream;
stream.startPC = pc;
stream.exeBranchInfo = entry;
stream.exeTaken = taken;
stream.predMetas[0] = meta;  // Must set meta from prediction phase

// Update predictor
tage->update(stream);
```
```

例如上面这种形式，记录了要怎么样调用这几个函数的顺序

5. 接下来就可以正式让AI 添加单元测试了：注意先不着急添加太多，而是让AI 给出添加建议，然后你选择几个来添加，它生成后再检查下是否合理
6. 添加好对应的编译选项，就可以用gem5 自带的GoogleTest 添加了

```cpp
# Add BTB test with minimal dependencies
GTest('btb.test', 
    'mockbtb.cc', 'btb.test.cc', '../stream_common.cc')
```

```cpp
git checkout dev-btb-rebase
scons build/RISCV/cpu/pred/btb/test/btb.test.debug -j100
./build/RISCV/cpu/pred/btb/test/btb.test.debug
```

编译运行都非常快，只用1s 就完成了

具体的可以参考readme, 位置在src/cpu/pred/btb/test/README.md

也可以参考gem5 dev-btb-rebase

│58d89ef 2025-03-06 Yan Yue               btb-pred: Add μRAS unit test suite  

这个commit， 查看第一个单元测试是如何添加的



### 后端如何添加单元测试
后端关键在于dyninst 的构建，稍微麻烦点，但也能做

参考fetch.cc 中如何构建dyninst 的

```cpp
memcpy(dec_ptr->moreBytesPtr(), fetchBuffer[tid] + blk_offset * instSize, instSize);    
    // 把fetchBuffer二进制搬到decode内部buffer中，这里instSize=4, 一条一条译码
    decoder[tid]->moreBytes(this_pc, fetch_addr);   // 是否需要更多字节
        if (dec_ptr->needMoreBytes()) {     // 能取就继续取下一条
            blk_offset++;} // 更新blk_offset

        do {
            staticInst = dec_ptr->decode(this_pc);      // 译码出具体静态指令了！
            DynInstPtr instruction = buildInst(
            tid, staticInst, curMacroop, this_pc, *next_pc, true);  
            // 得到DynInst, 并压入instBuffer/fetchQueue！
```

本质就是获取指令二进制，调用decode 译码，buildInst 生成inst

指令二进制如何获取： 让C++ 操作：输入一串汇编指令，写入test.s 文件，调用riscv 交叉工具链来编译，objudmp 获得指令二进制

这样原则上可以让AI 辅助并自动化做好，就可以生成想要的任何汇编代码组合，来生成后端的测试样例



### 后记
不要对AI 抱有太大幻想，它本质是加速我们写代码，打铁还需自身硬，自己不会的就别期望AI 写的比你好

我生成单元测试也是走了很多弯路，踩坑很多，但也比较有收获

附上我的一个.cursorrules

```markdown
这是一个GEM5 的模拟器项目，在官方分支上做了很多修改，便于和XiangShan 开源处理器对齐

每次回答前，先阅读以下内容：

1. 阅读configs/example/xiangshan.py 文件，了解XiangShan 开源处理器的基本配置
2. 阅读README.cn.md 文件，了解项目的基本信息
3. 阅读src/cpu/pred/README.md 文件，了解分支预测器的基本信息

每次回复前请说：好的，我明白了

请尽可能用思维链回答问题

请在回答中，使用中文，但是凡是涉及到代码的地方，请使用英文，生成代码给出合适的英文注释
生成commit message 时，使用英文！
```

