# FastSim v25 校准工作树

`fastsim-v25` 从官方 `v25.1.0.1`（`c8222cc67a`）整理
`/home/data00/yinhaolang/FastSim/gem5-fs` 在 2026-10-26 的当前源码。
`calibration-v25` 在其上移植 `calibration-v26` 的
`b9740f47a5..cefea43af2` 最终校准实现。原有 `calibration`、
`calibration-v26` 和 `stable` 保留；主工作树继续服务 v26 CPU 工程。

## 实现边界

保留完整 FastSim producer、KVM、native retirement/cache 观测和
non-temporal store。加入校准 ISA/uop、指令/FU 时序、zero idiom、
前端分组、fetch lookahead、ordinary load、TSO buffer/SBuffer 和 Ruby
顺序写入能力。v25 stdlib 及处理器时钟构造接口保留。

独立 v26 KVM/TASK/Marker/protobuf PC/分支 trace 层不导入；校准机制的
统计挂在对应 O3、FU 或 Ruby 统计组。LSQ request/entry 类型按校准所需
拆出，保留 v25 请求方法和 FastSim 的原生请求完成、转发与 split 响应观测。

## FST 接合

保留原有 89 个 v25 `OpClass` 编号，新增校准类别追加在尾部。
producer 映射新增类别到既有 FST FU 类别，JSONL 与直接 FST 使用同一映射。

STA 和 STD 两个执行成员均保留。STA 输出地址 ALU 记录；STD 输出逻辑
store 记录，带数据依赖及 STA 的显式依赖，使用 STA 的地址、大小、请求
flags 和 native 请求 identity。一对成员只输出一次 store 请求，宏指令
retirement 仍按最后一个真实 uop 记账。SBuffer 转发进入现有 native
StoreForward 终止路径。

FST 沿用最新 FastSim producer 的 v7 格式和 companion 契约。
zhonghaocheng 的旧 FastSim reader 尚未实现最新 producer 已有的
memory-request/retirement 扩展；此次使用
`/home/data00/yinhaolang/FastSim` 的当前 reader 验证输出。
旧 framework 的 reader 更新、采集配置及 SPR 参数接入属于后续工作。

## 构建

目标是 `build/X86_MESI_Three_Level/gem5.opt`。本次使用 GCC 11、
嵌入 Python 3.11.14、系统 protobuf/protoc 3.6.1，保留 v25 的 C++17。
SCons 由工作区 Python 驱动，共享头默认来自树内 `taogen/shared`。

```bash
source /home/data00/xuhaoen/.agent_cli_auth/env.sh
export CC=/opt/gcc-11/bin/gcc CXX=/opt/gcc-11/bin/g++
export PYTHON_CONFIG=/root/.pyenv/versions/3.11.14/bin/python3-config
export PATH="/usr/bin:$PATH" PKG_CONFIG_PATH=
export TAOGEN_SHARED="$PWD/taogen/shared"
export LD_LIBRARY_PATH="/root/.pyenv/versions/3.11.14/lib:/opt/gcc-11/lib64"
export LIBRARY_PATH=/opt/gcc-11/lib64
"$PYTHON" -m SCons -Q --ignore-style --linker=lld \
  "PYTHON_CONFIG=$PYTHON_CONFIG" \
  build/X86_MESI_Three_Level/gem5.opt -j16
```

更换 protobuf 的 configure 环境时须使用 `--config=force`，并通过选定
版本的 protoc 重新生成 proto；配置缓存不会跟踪 pkg-config 环境变化。
从空构建树生成全部对象后，单独执行一次最终二进制目标并检查其存在。
实际构建命令、探针源和输出留在各工作树 `build/migration/`。

## 验证记录

两个 `X86_MESI_Three_Level/gem5.opt` 均已生成。移植后的 Python 源码已解析，
新增差异通过 `git diff --check`。真机静态 ELF 指令和访存探针均以状态 0 完成。

- AtomicSimple、TimingSimple 和 O3 默认路径均通过指令与访存探针。
  覆盖 ADD/IMUL、32/64-bit DIV/IDIV、DIVSD、MOV/MOVSX、zero idiom、
  BMI/ADX/CRC/MOVBE，及跨行、部分写入、转发、non-temporal、原子和 SETcc store。
- 三种 `InstructionTiming` 均发生真实 admission；fetch lookahead 短运行通过。
- Ruby buffer（含两个 in-flight store）、Ruby SBuffer、前端 store/load-ALU
  分组及 ordinary-load service 均通过。Ruby SBuffer probe 发生 25 次
  offload 和实际转发；uncached SBuffer 背压 probe 发生 39 次 drain 拒绝，
  仍正确完成。修复了被阻塞 SQ 请求提前转移导致的失效重试。
- 当前 FastSim reader 完整读取基底、校准 serial 和校准 SBuffer 的真实 FST。
  三者均为 151 个宏指令、28 个 load、28 个 store；校准记录数为 204，
  与 O3 committed uop 统计一致。24 对 STA/STD 均保留地址依赖，未重复输出 store。
- 校准 JSONL 经当前 `convert-gem5` 转换后，与直接 FST 的全部 204 个
  64-byte hot records 逐字节一致。

FST probe 使用 gem5 的真实 SE 架构页表初始化；物理内存从 `0x1000` 开始，
为 producer 提供非零 CR3 身份。该结果不是 guest Linux/KVM 全系统运行验证。
permission-only prepare 的 buffer 路径在 Ruby 验证；零长度 prepare
不能经本次 uncached SystemXBar probe 路由。

部署工作树由 `/home/data00/xuhaoen/repos/simulators/gem5-cpu` 的共同 Git 管理：

| 目录 | 分支 |
| --- | --- |
| `/home/data00/zhonghaocheng/simulation-framework/gem5-fastsim-v25` | `fastsim-v25` |
| `/home/data00/zhonghaocheng/simulation-framework/gem5` | `calibration-v25` |

旧脏 `gem5` 完整改名保留为 `gem5-before-calibration-v25-20261026`；
新融合工作树先在 `gem5-calibration-v25-staging` 验证，再移动到部署目录。

验证只覆盖所运行的功能、配置接口和记账路径；不声称性能校准效果、
真实 workload transfer、完整 x86 指令集或完整内存序验收。
