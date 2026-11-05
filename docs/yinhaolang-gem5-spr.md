# yinhaolang gem5-spr 本地工作树

本记录对应 2026-11-05 的分支导入。来源是独立的 `gem5-spr` Git 仓库，
不是外层 `minesim` / FastSim 仓库中的同名分支。

## 位置与来源

| 项目 | 值 |
|---|---|
| 共享 Git 仓库 | `/home/data00/xuhaoen/repos/simulators/gem5-cpu` |
| 独立工作树 | `/home/data00/xuhaoen/repos/simulators/gem5-cpu/.worktrees/gem5-spr` |
| 本地分支 | `yinhaolang` |
| 来源仓库 | `/home/data00/yinhaolang/FastSim/gem5-spr` |
| 来源分支 | `refs/heads/yinhaolang` |
| 来源快照引用 | `refs/remotes/yinhaolang-local/yinhaolang` |
| 导入提交 | `fb8ef0f162127d629a3cbe02866270a8952c841e` |
| 部署基线 | `02119d168de477a2782f33d14ad229484ad17de4` |

新工作树有独立的 HEAD 和 index，提交对象及引用保存在 `gem5-cpu/.git` 中。
主工作树继续使用 `calibration-dev`，部署工作树继续使用 `calibration-deploy`。
本地 `.git/info/exclude` 中的 `/.worktrees/` 排除嵌套工作树目录，避免主工作树收录其源码。

`yinhaolang-local` 是来源快照的引用命名空间，未配置同名 remote 或自动上游。
导入使用明确的本地来源路径，没有修改现有 Git 配置。后续源码修改属于本地
`yinhaolang` 分支；来源快照引用仅由显式 fetch 更新。

## 提交关系

来源分支在部署基线之后有 17 个提交。第一个提交
`aa8ff90980ed4ad9531e2ee247361fe5ba7007e1` 的唯一父提交就是上述部署基线。
它先保存已有本地 SPR 状态，随后接入并扩展采集链路。
这一关系证明提交基线一致，不证明来源曾复制本机目录或包含本机当前未提交改动。

```text
02119d168d  feat(o3, x86): complete O3 modeling and fixed-work calibration snapshots
  aa8ff90980  Preserve SPR calibration and retired frontend baseline before collector migration
  7231d7d2b7  Connect legacy FS collection protocol to native SPR target
  27ef513696  Preserve SPR DSB terminal evidence and observe true collection start
  2c8250127d  Capture ASID-scoped static maps and native task lifecycle clocks
  b69ba85b05  Bootstrap native ROI counters and prove initial task prefixes
  9229fe5d03  Observe last retained retirement for native CPI endpoints
  afb85b808a  Add opt-in DDR5 large-memory capacity for native SPR speed capture
  f7abcbaec1  Collect source-bound static uop and functional memory facts
  454a5f4d2b  Keep the functional measurement epoch stable across repeated BEGIN
  94cb06d613  Qualify split requests against bound per-page physical fragments
  9fa6326969  Capture immutable instruction translation origins with explicit contexts
  afcbd5641f  Preserve data translation origins with explicit retirement context
  18002f6cbb  Observe accepted device bypass lifecycle and reclaim abandoned identities
  8ccad4329c  Capture immutable core functional facts without changing SPR execution
  7e121b8a2e  Capture functional MMU controls with retained ownership and cut diagnostics
  1ef38b9055  Capture architectural boundary lineage and version functional facts
  fb8ef0f162  Isolate functional syscall timing and derive store pairs
```

## 查看与显式更新

先加载共同环境：

```bash
source /home/data00/xuhaoen/.agent_cli_auth/env.sh
```

在主仓库即可查看导入的历史：

```bash
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu worktree list
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu log --oneline yinhaolang
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu log --reverse --oneline 02119d168d..yinhaolang
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu show -s --format=fuller aa8ff90980
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu/.worktrees/gem5-spr status
```

需要同步来源时，先获取来源分支，再检查本地工作树与分支差异：

```bash
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu fetch --append --no-tags --no-recurse-submodules \
  /home/data00/yinhaolang/FastSim/gem5-spr \
  refs/heads/yinhaolang:refs/remotes/yinhaolang-local/yinhaolang
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu/.worktrees/gem5-spr status --short
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu/.worktrees/gem5-spr log \
  --left-right --oneline yinhaolang...refs/remotes/yinhaolang-local/yinhaolang
```

确认本地工作树干净且需要前进到来源提交时，执行：

```bash
git -C /home/data00/xuhaoen/repos/simulators/gem5-cpu/.worktrees/gem5-spr merge \
  --ff-only refs/remotes/yinhaolang-local/yinhaolang
```

来源迁移说明位于
`/home/data00/yinhaolang/FastSim/docs/spr-collection-migration-20261103.md`；
分支自带采集说明位于新工作树的 `configs/fastsim/README.md`。
本次只导入 Git 历史与受版本控制的源码，没有初始化依赖子模块、构建或运行仿真。

## 既有登记说明

共享 Git 已有一条指向缺失路径
`/home/data00/xuhaoen/repos/calibration/cpu_microarchitecture/build/spr-port-alu/deployment-source`
的旧 worktree 登记。当前 Git 2.20.1 创建新工作树时会因该路径缺失而失败。
本次临时移开该登记，创建后已原样恢复；未清理旧登记。

完成检查确认新工作树干净、分支与来源快照一致，17 个新增提交均已导入。
核对期间研发树 `src/cpu/o3/lsq_unit.cc` 出现并发更新，原开发改动摘要因此发生变化；
本次操作未写入或回退该文件，也未切换研发及部署工作树的分支。
