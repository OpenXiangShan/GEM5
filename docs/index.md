# 欢迎

欢迎使用香山GEM5模拟器文档！

GEM5是一个模块化的离散事件驱动的计算机系统架构模拟器平台，参考：[官方GEM5](https://www.gem5.org/)

香山GEM5是专门为香山处理器定制的GEM5模拟器，相比官方GEM5：

- 性能和SPEC CPU 2006基准测试上与昆明湖处理器性能相当，误差在5%以内！
- 支持香山特有的格式和功能
- 包含多个香山特有的功能增强

查看[介绍](introduction.md)了解更多信息。

## 当前文档结构

当前网站中的文档大致分为以下几类：

- `introduction.md`、`quick_start.md`
  - 面向新用户，介绍项目背景、基本使用方式和入门流程。
- `design-docs/`
  - 面向设计理解与架构阅读。
  - 这部分文档更偏“为什么这样设计”“约束是什么”“模块之间如何分工”。
- `Gem5_Docs/`
  - 面向实现细节、代码路径和局部机制说明。
  - 这部分文档通常更贴近具体实现和源码结构。
- `tools/`
  - 面向工具链、辅助脚本和相关使用流程。
  - 这部分文档更适合作为开发和分析过程中的工具参考。

如果你想先理解 Kunminghu v3 前端 / BPU 的顶层设计动机、关键约束和模块分工，建议优先阅读：

- [设计文档索引](design-docs/README.md)
- [Kunminghu Frontend 设计文档索引](design-docs/frontend/README.md)
- [XS-GEM5 SMT 设计说明](design-docs/smt/README.md)

这部分目前重点覆盖：

- Kunminghu v2 `FTB` 到 Kunminghu v3 `BTB` 的顶层演进
- `PHR`、`mBTB`、`BTBTAGE`、`MGSC`、`uBTB`、`AheadBTB`、`MicroTAGE` 等模块的设计取舍
- SMT 的线程隔离、共享资源、流水线流控、访存可见性和 FS 验证路径

如果你已经知道大致设计背景，想继续看更细的实现说明，则可以再进入 `Gem5_Docs` 目录阅读对应主题文档。

## 快速开始

查看[快速开始](quick_start.md)部分了解更多信息。

!!! note
    本项目正在积极开发中。

## 如何添加新文档

在docs目录下添加新的md文件
然后运行如下命令在本地预览网页：

```bash
touch docs/frontend/test.md

# 本地预览
pip install -r docs/requirements.txt
mkdocs serve

# 提交PR
git add docs/frontend/test.md
git commit -m "添加新文档 [skip ci]"    # 跳过CI检查
git push
``` 
