# server

这里暂时只保留阶段 0 的职责说明，不提前实现直播业务服务。

未来这个目录负责：

- 开播鉴权
- 创建直播间
- 返回 `live_id / stream_key / rtmp_push_url / play_url`
- 为连麦双方签发 `livekit_url / room_name / token`
- 接 SRS 回调，维护直播状态
- Redis 缓存
- SSE 推送给观众端
- WebSocket 信令给主播端
- 房间在线人数与在线成员列表同步
- 连麦申请、主播邀请、接受、拒绝、结束

阶段 0 不实现这些接口，只先把边界定清楚。
