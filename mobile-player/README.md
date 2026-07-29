# mobile-player 手机端拉流播放器

基于 HLS / HTTP-FLV 的手机端直播拉流页面，配合 SRS 使用。

## 使用方式

### 方式一：双击打开 (本地预览)

直接用浏览器打开 `index.html`，输入 HLS 地址即可播放。

### 方式二：局域网手机访问 (推荐)

启动本地 HTTP 服务器，手机扫码或输入地址访问：

#### 用 Python（如果已安装）
```powershell
cd mobile-player
python -m http.server 3000
```

#### 用 Node.js（如果已安装）
```powershell
cd mobile-player
npx serve . -p 3000
```

然后在手机浏览器访问 `http://<本机IP>:3000`

### 方式三：部署到 SRS www 目录

将 `index.html` 放入 SRS 的 www 目录，手机直接访问 `http://<srs-ip>:8080/`

---

## 播放地址格式

假设主播端推流地址为：
```
rtmp://192.168.3.28:1935/live/room123
```

对应的拉流地址：
| 协议 | 地址 |
|------|------|
| HLS | `http://192.168.3.28:8080/live/room123.m3u8` |
| HTTP-FLV | `http://192.168.3.28:8080/live/room123.flv` |

## SRS HLS 配置（确认 SRS 已开启）

在 `srs.conf` 中确保包含：
```nginx
vhost __defaultVhost__ {
    hls {
        enabled on;
        hls_fragment 2;
        hls_window 10;
    }
    http_remux {
        enabled on;
        mount [vhost]/[app]/[stream].flv;
    }
}
```
