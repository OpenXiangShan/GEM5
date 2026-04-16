# 欢迎

欢迎使用香山GEM5模拟器文档！

GEM5是一个模块化的离散事件驱动的计算机系统架构模拟器平台，参考：[官方GEM5](https://www.gem5.org/)

香山GEM5是专门为香山处理器定制的GEM5模拟器，相比官方GEM5：

- 性能和SPEC CPU 2006基准测试上与昆明湖处理器性能相当，误差在5%以内！
- 支持香山特有的格式和功能
- 包含多个香山特有的功能增强

查看[介绍](introduction.md)了解更多信息。

## 设计文档入口

如果你想先理解 Kunminghu v3 前端 / BPU 的顶层设计动机、关键约束和模块分工，建议优先阅读：

- [Kunminghu Frontend 设计文档索引](design-docs/frontend/README.md)

这部分文档更偏“为什么这样设计”，重点覆盖：

- Kunminghu v2 `FTB` 到 Kunminghu v3 `BTB` 的顶层演进
- `PHR`、`mBTB`、`BTBTAGE`、`MGSC`、`uBTB`、`AheadBTB`、`MicroTAGE` 等模块的设计取舍

与之对应，`Gem5_Docs` 目录中的文档继续保留更多实现细节、代码路径和局部说明，更适合作为补充参考。

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
