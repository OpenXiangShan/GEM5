# 欢迎

欢迎使用香山GEM5模拟器文档！

GEM5是一个模块化的离散事件驱动的计算机系统架构模拟器平台，参考：[官方GEM5](https://www.gem5.org/)

香山GEM5是专门为香山处理器定制的GEM5模拟器，相比官方GEM5：

- 性能和SPEC CPU 2006基准测试上与昆明湖处理器性能相当，误差在5%以内！
- 支持香山特有的格式和功能
- 包含多个香山特有的功能增强

查看[介绍](introduction.md)了解更多信息。

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