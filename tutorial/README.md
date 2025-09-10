1. 进入 gem5 主目录
2. 用关闭 ittage 的 gem5 版本，跑一个测试间接跳转的小测试，得到 ipc
`bash tutorial/ittage_part1.sh`
3. 用开启 ittage 的 gem5 版本，跑一个测试间接跳转的小测试，得到 ipc
`bash tutorial/ittage_part2.sh`
4. 用 hotloop 分析工具分析间接跳转小测试的热点函数
`bash tutorial/ittage_hotloop.sh`
5. 用 perfcct trace 分析关闭 ittage 的 gem5 版本的指令预测情况
`bash tutorial/ittage_trace1.sh`
6. 用 perfcct trace 分析开启 ittage 的 gem5 版本的指令预测情况
`bash tutorial/ittage_trace2.sh`