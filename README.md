# duanshipin-live-studio

这是与 `duanshipin-pingtai` 并行的新项目目录，定位是主播侧独立程序与后续直播业务服务的承载仓。

当前只完成 `阶段 0：现有代码盘点与结构对齐`，不提前实现单人开播闭环。

## 当前阶段目标

- 盘点 `duanshipin-pingtai` 里已经可复用的观众侧、服务端、部署资产
- 盘点同级历史实验工程里可借鉴但不应直接搬运的代码
- 明确新项目的目标架构、增量实施路线、模块边界
- 建立阶段 0 的项目骨架与报告文档

## 现在有哪些内容

- `docs/current_architecture.md`
  - 现状盘点、可复用资产、缺失模块、风险边界
- `docs/target_architecture.md`
  - 目标架构、模块拆分、数据流路径
- `docs/incremental_plan.md`
  - 阶段化推进路线
- `docs/stage_00_report.md`
  - 阶段 0 报告
- `apps/stage0_probe`
  - 一个仅用于阶段 0 验证的极小控制台程序
- `src/*`
  - 仅放接口骨架与 TODO，不包含阶段 1 的真实媒体实现

## 为什么阶段 0 只做骨架

这个项目属于音视频学习核心任务。当前最重要的是先把链路、模块边界和复用关系讲清楚，而不是一上来把采集、编码、RTMP、SRS、WebRTC 全部堆进一个新程序里。

## 阶段 0 验证

优先方式：

```powershell
cmake -S . -B build
cmake --build build
.\build\apps\stage0_probe\stage0_probe.exe
```

如果当前终端里没有 `cmake` 命令，可用仓库自带脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_stage0_probe.ps1
```

## 下一阶段入口

只有在你确认阶段 0 的架构、复用策略、增量路线都合理之后，才进入阶段 1：

`摄像头视频采集 -> H.264 编码 -> FLV 视频 tag -> RTMP 推流 -> SRS -> HTTP-FLV -> duanshipin-pingtai 观众端`
