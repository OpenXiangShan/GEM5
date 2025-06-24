# 测试用例分析流程

用例标签：一句话简单总结这个测试测的是什么

用例软件行为描述：测试在做什么？具体行为是什么？

理想IPC：理论上最高ipc是多少

理想模型IPC：使用ideal脚本跑出的模型理想ipc（cliff.py --ideal-kmhv3）（commit版本）

模型IPC：使用非ideal脚本跑出的模型理想ipc（cliff.py）（commit版本）

RTL IPC：使用RTL跑出的ipc （关预取器？）（commit版本）

DIFF：模型理想IPC以及RTL IPC的差异

mismatch描述：什么原因造成了现在这个差异

assignee：测试分析的负责同学（有疑问就找他）

comment：测试注意事项有哪些，可以写测试XXX地方仍需要改进，也可以写发现了架构XXXX需要优化，等等

