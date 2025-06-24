# vscode 配置代码跳转和调试

## vscode 配置Gem5代码跳转
#### 方法一
首先需要CMakeLists.txt, 放入gem5顶层目录

```bash
cmake_minimum_required(VERSION 2.8.13)
project(gem5)

#Bring the headers, such as Student.h into the project
FIND_PACKAGE( Boost 1.6 )
include_directories(${Boost_INCLUDE_DIRS})
include_directories(src)
include_directories(build/RISCV)
include_directories(ext/softfloat)

#Can manually add the sources using the set command as follows:
#set(SOURCES src/mainapp.cpp src/Student.cpp)

#However, the file(GLOB...) allows for wildcard additions:

file(GLOB_RECURSE cpu_sources src/cpu/*.cc src/cpu/*.hh)
file(GLOB_RECURSE pred_sources src/cpu/pred/*.cc src/cpu/pred/*.hh)
file(GLOB_RECURSE ff_sources src/cpu/forwardflow/*.cc src/cpu/forwardflow/*.hh)
file(GLOB arch_sources src/arch/*.cc src/arch/*.hh)
file(GLOB_RECURSE rv_sources src/arch/riscv/*.cc src/arch/riscv/*.hh)
file(GLOB_RECURSE softfloat_sources ext/softfloat/*.c ext/softfloat/*.h)

file(GLOB_RECURSE other_sources
        src/base/*.cc   src/base/*.hh
        src/mem/*.cc    src/mem/*.hh
        src/sim/*.cc    src/sim/*.hh
        )
file(GLOB_RECURSE build_sources
        build/RISCV/enums/*.cc build/RISCV/enums/*.hh
        build/RISCV/config/*.cc build/RISCV/config/*.hh
        build/RISCV/params/*.cc build/RISCV/params/*.hh
        build/RISCV/debug/*.cc build/RISCV/debug/*.hh
        build/RISCV/proto/*.cc build/RISCV/proto/*.hh build/RISCV/proto/*.h
        )

set(sources ${cpu_sources} ${arch_sources} ${rv_sources}
        ${softfloat_sources}
        ${other_sources} ${build_sources}
        ${pred_sources} ${ff_sources})

add_definitions(-DTRACING_ON=true)

add_compile_definitions(__CLION_CODING__)
add_executable(gem5 ${sources})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
```

然后安装cmake, cmake tools 这两个vscode 插件

ctrl+shift+p   输入 show cmake,会要求配置Gcc 路径，选择自己的x86 gcc 就行

确认识别到了CMakeLists.txt, 没有识别就重启vscode, 然后点击重新配置

![](images/1731485400017-44da2345-da11-4757-b8c3-e3ca44ff3bca.png)

最后会在build 目录 下生成compile_commands.json（也就是所有c++文件编译关系）

注意，这个CMakeLists.txt 描述了构建关系，但是不用于构建，仍然用scons 来构建。

最后安装clangd 插件（clangd 是c++ language server， 读取json文件生成索引，来跳转）

![](images/1731485816062-e24a090c-4021-4049-aa9b-0ddfcca6cfd8.png)

报错：它的意思是不用默认c++插件intelliSence 跳转，用clangd 跳转，这是合理的

理论上讲安装clangd后C++插件的intellisence会自动被忽视，但有时候还是会与clangd冲突导致代码跳转失效，这时候可以打开setting.json（ctrl+shift+p 输入 Open User Settings(JSON)），然后加上这行：

```plain
"C_Cpp.intelliSenseEngine": "disabled",
```

这样就可以使用clangd的跳转，同时使用C++插件的调试界面了

ctrl+shift+p ： clangd: restart language server

![](images/1731485651230-64e31b0b-33ee-4f72-95f3-098858e3ae65.png)

确认clangd 正确加载json 文件，并且底部开始索引，说明正确配置了，可以开始愉快的c++ 代码跳转了！

![](images/1731485671590-7ea9a90f-f832-4b65-9814-203f70406cd6.png)

gem5 python 代码跳转还有待优化



#### 方法二
直接用scons 自带的方法来生成compile_commands.json 

目前还只在dev-btb-rebase 分支上，等之后会合入主线（目前已合入主线）

```bash
# 正常编译一次gem5
scons build/RISCV/gem5.opt --gold-linker -j100    
# 生成数据库文件
scons build/RISCV/compile_commands.json   
du -sh build/RISCV/compile_commands.json                                                 
     55M     build/RISCV/compile_commands.json  # 生成文件很大
# 软链接到主目录，方便clangd 找到
ln -s build/RISCV/compile_commands.json .
# clangd: restart language server 即可
```

同样按照方法一：安装clangd 来生成索引就好。

这个生成的跳转回默认跳转到build/RISCV/ 下的文件，本质是src 文件中的符号链接文件，每次返回去找原本文件会比较麻烦，可以安装vscode Symlink Follow插件，当打开符号链接文件时候，能提示是否自动跳转到源文件。这个插件每次跳转都跳出弹窗问是否跳转过去，点开插件设置勾选第一个选项就可以默认跳转了。

![](images/1742522739336-735e260f-ede2-4eee-9721-13dfe9fd5a56.png)

#### 方法三
使用 bear 来直接生成 compile_commands.json

```shell
bear -- scons build/RISCV/gem5.opt --gold-linker -j100
```







最后，安利下cursor 编辑器，[https://www.cursor.com/](https://www.cursor.com/), 比vscode 更加好用



### vscode 配置内置gdb
安装c/c++ 插件

在gem5 根目录创建.vscode/launch.json 文件

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "(gdb) 启动",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build/RISCV/gem5.debug",
      "args": [
        "--debug-break=569430",
        "--debug-flags=DecoupleBP",
        "${workspaceFolder}/configs/example/xiangshan.py",
        //   "--ideal-kmhv3",
        "--raw-cpt",
        "--generic-rv-cpt=/nfs/home/yanyue/tools/nexus-am-xs/tests/cputest/build/brnum2-riscv64-xs.bin",
      ],
      "stopAtEntry": false,
      "cwd": "${fileDirname}",
      "environment": [],
      "externalConsole": false,
      "MIMode": "gdb",
      "setupCommands": [
        {
          "description": "为 gdb 启用整齐打印",
          "text": "-enable-pretty-printing",
          "ignoreFailures": true
        },
        {
          "description": "将反汇编风格设置为 Intel",
          "text": "-gdb-set disassembly-flavor intel",
          "ignoreFailures": true
        },
        {	// 默认输出为16进制，可以不用
          "description": "设置输出为16进制",
          "text": "set output-radix 16",
          "ignoreFailures": true
        }
      ]
    }
  ]
}
```

通过修改args 中的参数就能运行对应的gem5 命令

如果需要某些环境变量，建议放到.zshrc/ .bashrc 中，这样vscode 启动gdb 时候会默认读取这些环境变量

然后在fetch::tick() 设置一个断点

![](images/1738739817474-6965d547-51bf-4fb4-a05b-388e9354656f.png)

按下F5， 或者下面启动gdb

![](images/1738739872412-99efbb3a-23ea-4926-81e4-b57f8970c06d.png)

如果需要输入gdb 命令，在下面的调试控制台，添加-exec 前缀来输入gdb 命令

![](images/1738739950000-cc27dd6f-ffbe-4c16-8c42-06e43ad6a6af.png)

就可以愉快的调试了

