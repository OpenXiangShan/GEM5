欢迎使用香山GEM5模拟器文档！
===================================

GEM5是一个模块化的离散事件驱动的计算机系统架构模拟器平台，参考：https://www.gem5.org/

香山GEM5是专门为香山处理器定制的GEM5模拟器，相比官方GEM5：

- 性能和SPEC CPU 2006基准测试上与昆明湖处理器性能相当，误差在5%以内！
- 支持香山特有的格式和功能
- 包含多个香山特有的功能增强

查看 :doc:`introduction` 了解更多信息


快速开始
--------------------------------

查看 :doc:`quick_start` 部分了解更多信息

.. note::

   本项目正在积极开发中。

本地编写文档并测试
--------------------------------

.. code-block:: bash

   pip install -r docs/requirements.txt

   cd docs
   make html

   cd build/html
   python -m http.server

目录
----

.. toctree::
   :maxdepth: 2

   introduction
   quick_start
   frontend/test
   markdown_example
