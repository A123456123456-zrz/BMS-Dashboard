# tunnel/ 隧道配置说明

**本目录在本地保持为空(仅占位)** —— cloudflared 隧道配置只部署在阿里云 ECS 上。

## 为什么本地没有隧道文件

- BMS 服务器只部署在 **ECS <YOUR_ECS_IP>**(本地电脑不做服务器)
- 隧道配置文件 `config.yml`、凭据文件 `27c0e102-74d6-4b5b-91cd-50b021f09ba4.json`、`cloudflared.exe` 均在 ECS 的 `/opt/bms-dashboard/tunnel/`
- 本地此目录保留仅为保持项目结构一致,请勿在此放置凭据(避免密钥入库)

## ECS 侧隧道速览

| 项 | 值 |
|---|---|
| 服务 | `cloudflared.service`(systemd,开机自启 + 崩溃重启) |
| 启动参数 | `--protocol quic --edge-ip-version 4 --ha-connections 8 --retries 30 --region us` |
| 隧道域名 | `https://bms0605.dpdns.org`(→ Flask 5000) |
| ingress | `http://127.0.0.1:5000`(必须是 127.0.0.1,不能写 localhost——Windows 才需要 localhost,Linux 无此问题) |

## 常用运维命令(在 ECS 上执行)

```bash
systemctl status cloudflared      # 查看服务状态
journalctl -u cloudflared -f      # 实时日志
systemctl restart cloudflared     # 重启隧道
```

> 历史备注:ECS→Cloudflare 走 QUIC(UDP 7844)稳定;TCP 7844(http2)曾被干扰。版本已锁定 2026.7.3。
