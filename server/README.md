# server

这个目录不再承载本地 Node.js probe signaling server。

当前项目约定如下：

- 实际业务服务端工程位于 `../duanshipin-pingtai/server`
- 服务端语言固定为 `Go`
- 服务端部署位置固定为虚拟机
- `duanshipin-live-studio` 只负责 Windows 主播端和 probe 客户端实现

当前已经确认的服务端形态是“现有平台 Go 后端继续扩展连麦能力”，而不是在本仓库里单独再维护一套 JS 信令服务。结合现有平台工程，后端已经具备并可继续复用的方向包括：

- HTTP API 入口
- 环境变量配置
- MySQL
- SSE 事件推送
- 媒体平台已有的鉴权和业务模型
- Redis 可选接入

其中 Redis 目前不是 probe 联调和最小连麦闭环的硬依赖，但后续很适合承接这些能力：

- 在线成员状态缓存
- 邀请超时和状态同步
- 多实例之间的房间事件广播

连麦相关能力应继续落在 Go 服务端，至少包括：

- 房间在线成员列表与在线人数同步
- 连麦申请、邀请、接受、拒绝、取消、结束
- WebSocket 信令转发
- `rtc.offer`
- `rtc.answer`
- `rtc.ice-candidate`
- `STUN/TURN` 配置下发
- 直播间状态与上麦状态同步

本目录当前只保留两类内容：

- 服务端职责边界说明
- 客户端需要遵守的接口与消息约定

如果后续需要真正补服务端实现，请直接在 `../duanshipin-pingtai/server` 的 Go 工程里扩展，不要在这里重新起一个 Node 占位版本。
