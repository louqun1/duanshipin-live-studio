# 阶段 0 报告

## 1. 当前阶段目标

- 盘点 `duanshipin-pingtai` 与同级历史实验中的现有资产
- 明确哪些模块能直接复用，哪些只能参考，哪些必须新写
- 给出新主播端项目的目标结构与增量实现顺序
- 建立新项目目录和接口骨架

## 2. 实际实现范围

### 已完成

- 新建 `duanshipin-live-studio` 目录
- 输出当前架构盘点文档
- 输出目标架构文档
- 输出增量实施计划
- 建立阶段 0 骨架目录
- 建立 `stage0_probe` 最小可执行程序
- 建立若干接口级骨架：
  - `VideoCaptureSource`
  - `VideoEncoder`
  - `FlvMuxer`
  - `RtmpPublisher`
  - `LiveSessionController`

### 明确没做

- 没有实现任何摄像头采集
- 没有实现 H.264/AAC 编码
- 没有实现 FLV 打包
- 没有实现 RTMP 建连、握手、发送
- 没有实现任何新直播业务 API
- 没有实现任何 WebRTC 代码

## 3. 核心模块图 / 数据流说明

### 3.1 当前可复用的观众侧链路

```text
SRS HTTP-FLV
-> duanshipin-pingtai LivePlayerController
-> LivePlayerSession
-> HttpFlvStreamReader
-> FlvDemuxer
-> FlvAudioDecoder / FlvVideoDecoder
-> SdlAudioOutput / VideoFrameQueue
-> VideoOpenGLWidget
```

### 3.2 新项目目标主链路

```text
VideoCaptureSource
-> VideoEncoder
-> FlvMuxer
-> RtmpPublisher
-> SRS
-> HTTP-FLV
-> duanshipin-pingtai viewer
```

### 3.3 阶段 0 的意义

阶段 0 不是实现这条链路，而是先把：

- 链路上有哪些模块
- 哪些模块在旧项目里已经存在
- 哪些模块必须新写
- 每层边界怎么划

先定清楚。

## 4. 关键路径说明

### 4.1 现有项目里已经跑通的路径

当前已经可演示的直播观看路径是：

```text
外部 RTMP 推流
-> SRS
-> HTTP-FLV
-> duanshipin-pingtai Qt viewer
```

### 4.2 新项目要补的半条路径

新项目首先补的是：

```text
主播侧采集
-> 编码
-> FLV tag
-> RTMP 发送
```

### 4.3 为什么不直接复用旧 `rtmp-publish-20210903`

因为它虽然有参考价值，但主发送层依赖 `librtmp`，不符合“本阶段需要看清 RTMP 细节”的约束。

## 5. 运行方式

### 5.1 阶段 0 骨架验证

优先方式：

```powershell
cd d:\itffmpeg\av_media\online\duanshipin-live-studio
cmake -S . -B build
cmake --build build
.\build\apps\stage0_probe\stage0_probe.exe
```

如果当前终端没有 `cmake` 命令，可执行：

```powershell
cd d:\itffmpeg\av_media\online\duanshipin-live-studio
powershell -ExecutionPolicy Bypass -File .\scripts\build_stage0_probe.ps1
```

## 6. 验证方式

### 6.1 我应该如何检查

- 检查 `README.md`
- 检查 `docs/current_architecture.md`
- 检查 `docs/target_architecture.md`
- 检查 `docs/incremental_plan.md`
- 检查 `docs/stage_00_report.md`
- 运行 `stage0_probe`

### 6.2 预期现象

- 新项目目录已经创建
- 阶段 0 文档完整
- 能明确看到“直接复用 / 仅参考 / 必须新写”的边界
- `stage0_probe` 能打印当前阶段状态与模块骨架

## 7. 关键日志片段解释

本阶段没有进入媒体功能实现，所以没有新增运行态媒体日志。

阶段 0 唯一新增的运行输出来自 `stage0_probe`：

```text
duanshipin-live-studio stage 0 probe
Project status: architecture alignment only
Modules declared:
...
No capture/encode/RTMP logic is implemented in stage 0.
```

它的意义不是证明推流完成，而是证明：

- 新项目已独立创建
- 阶段 0 骨架可被构建和执行
- 接下来可以在这个目录内继续按阶段推进

补充说明：

- 我本次实际验证走的是 `g++ + scripts/build_stage0_probe.ps1` 这一条，因为当前 shell 环境里 `cmake` 不在 `PATH`

## 8. 当前缺陷 / 风险

### 8.1 还没有真实推流能力

这是预期内缺陷，因为阶段 0 本来就不实现推流。

### 8.2 旧实验代码复用边界仍需要你确认

我当前的建议是：

- 不直接搬 `rtmp-publish-20210903` 的 `librtmp` 路线
- 只把它作为参考资料

如果你想保留其中某些采集或编码实现，也建议只局部迁移，不整块搬。

### 8.3 业务服务暂未拆仓

现在 `server/` 只写了职责说明，直播业务 API 还未开始。

## 9. 是否建议进入下一阶段

建议进入阶段 1，但前提是你先确认下面 3 个决定：

1. 新项目目录名 `duanshipin-live-studio` 是否接受
2. 阶段 1 是否先走“摄像头采集”，还是先走“本地文件模拟采集后再切摄像头”
3. `RtmpPublisher` 是否坚持不直接依赖 `librtmp` 作为正式主实现

如果这 3 点你都认可，就可以开始阶段 1。
