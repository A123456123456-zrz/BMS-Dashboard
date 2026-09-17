# BMS 手机 App 监控与下发指令方案（说明书第 07 篇）

- 版本: v1.0 | 日期: 2026-08-10 | 作者: BMS Team
- 目的: 评估"手机 App 实时监控 + 远程下发指令"的可行性与实现路线

---

## 1. 结论先行

**完全可行，且成本很低** —— 因为后端能力已经全部就绪：

| 已有能力 | 现状 | App 可直接复用 |
|----------|------|----------------|
| 实时数据 | `tools/dashboard/backend/app.py` REST API + SocketIO/WebSocket 推送 | ✅ 直接调 `/api/*` 或连 SocketIO |
| 历史/报表 | `/api/history` `/api/report` `/api/export.csv` | ✅ 直接调 |
| 命令下发 | `/api/cmd`（operator+ 权限，白名单校验） | ✅ 直接 POST |
| 参数下发 | `/api/params`（POST，固件 `set_param` 闭环） | ✅ 直接 POST |
| 公网 HTTPS | Cloudflare 隧道 `http://<YOUR_ECS_IP>:5000` | ✅ App 远程访问入口 |
| 登录鉴权 | 多用户 RBAC（admin/operator/viewer）+ session | ✅ 复用 |
| 移动端形态 | **PWA**（`manifest.json` 已配，可"添加到主屏幕"） | ✅ 零开发直接用 |

> 也就是说：**App 只是现有 Web 服务的一个"前端壳"**，后端、云端、固件链路一行都不用改（除 06 篇 D1 控制命令缺陷建议先修）。

---

## 2. 三条可选路线（按成本从低到高）

### 路线 A：PWA 直接手机化（推荐，0 开发成本，当天可用）

- 现状：`manifest.json` + favicon + theme-color 已配置，**手机浏览器打开 `http://<YOUR_ECS_IP>:5000` → 菜单"添加到主屏幕"**，即可获得全屏 App 体验。
- 优点：零开发、自动与网页功能同步（实时曲线/控制面板/告警推送/多设备）、无需上架审核。
- 缺点：不是原生应用商店 App；推送依赖浏览器通知（iOS 有限制）。
- 适合：自用/内部运维监控。

### 路线 B：微信小程序 / uni-app（推荐做对外产品）

- 后端不变，前端用 **uni-app**（Vue 语法，一套代码可出：微信小程序 + 安卓/iOS App + H5）。
- 数据交互全部复用现有 REST API：
  - 登录：POST 现有登录接口拿 session（小程序需维护 cookie 或用 token 改造）
  - 实时：`/api/status` 5s 轮询，或连 SocketIO
  - 指令：`/api/cmd` `/api/params`（需 operator+ 账号）
- ⚠️ 前置条件（微信小程序特有）：
  1. 域名需 **ICP 备案 + HTTPS**（当前 Cloudflare 隧道域名需确认备案状态；未备案可先做 H5/安卓 iOS App，或换已备案域名）
  2. 在小程序后台把域名加入 **request/WebSocket 合法域名**白名单
- 优点：微信生态触达、审核上架渠道成熟；uni-app 一次开发多端发布。
- 适合：对外商用、团队多成员远程监控。

### 路线 C：原生 App（Flutter / React Native）直接对接现有 API

- 后端不变，App 端调现有 `/api/*` + SocketIO，UI 完全自绘。
- 优点：体验/性能最好，可做本地 BLE（蓝牙）直连设备等高级能力。
- 缺点：开发量最大，需双端维护 + 应用商店上架。
- 适合：要深度定制 UI、要离线 BLE 直连、商业化 App。

---

## 3. App 需要的最小 API 清单（全部已存在）

| 功能 | 接口 | 说明 |
|------|------|------|
| 登录 | `POST /login`（表单） | 用户名+密码，返回 session |
| 实时状态 | `GET /api/status` | 总压/电流/SOC/SOH/温度/故障码/单体电压 |
| 历史曲线 | `GET /api/history?minutes=&limit=` | 曲线数据 |
| 实时推送 | SocketIO `bms_data` / `bms_fault` | 或退化 5s 轮询 |
| 控制指令 | `POST /api/cmd` | `set_charge/set_discharge/set_balance/set_relay/restart/clear_alarm...`（operator+） |
| 参数设置 | `POST /api/params` | 保护阈值/串数/SOC 算法等 41 键（operator+） |
| 报表 | `GET /api/report?range=` | 日报/周报/月报 |
| 一键诊断 | `GET /api/diag` | 全链路体检 |

---

## 4. 推荐实施步骤（如果选路线 B 或 C）

1. **先修 06 篇 D1**（控制命令 `enable` 参数丢失）——否则 App 里"远程开启"一样是坏的。
2. 保持现有 `app.py` 不动，新增/确认 CORS 或同源部署；小程序如跨域需在 `dashboard.env` 配置允许域名。
3. 用 uni-app 建工程 → 封装 `api.js`（登录/状态/历史/命令/参数）→ 页面：登录页、仪表盘、单体电压、控制面板、告警列表、设置页。
4. 联调：浏览器 H5 先跑通 → 微信开发者工具跑小程序 → 打包安卓/iOS。
5. 上线：小程序走微信审核；App 走应用商店；PWA 直接发布。

---

## 5. 与现有功能的差异点（App 特有注意）

- **推送**：小程序用"订阅消息"（需后端调微信 API），PWA 用浏览器通知，原生 App 用厂商推送（FCM/厂商通道）——三者的告警推送通道互不相同，不能直接复用 `alert_push.py` 的企业微信通道。
- **鉴权**：PWA/浏览器直接复用 session；小程序/原生 App 建议后续改造为 token（JWT）或维持 session + 维护 cookie。
- **安全性**：控制命令在公网可达，建议 App 端强制二次确认（输入确认码/指纹）后再下发 `set_charge` 等。

---

## 6. 建议结论

| 场景 | 推荐路线 | 工期 |
|------|----------|------|
| 自己/内部运维 | A（PWA 直接手机化） | 0 天 |
| 对外产品/团队监控 | B（uni-app 一套多端） | 1~2 周 |
| 商用深度定制 + BLE | C（Flutter 原生） | 1 个月+ |
