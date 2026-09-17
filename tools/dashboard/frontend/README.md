# frontend/ — BMS Dashboard 前端

本目录存放 Web 前端文件(页面模板 + 静态资源),由 `backend/app.py`(Flask)加载:
- `templates/index.html`   主监控页面(总览/电芯/趋势/控制/告警/报表/设置)
- `templates/login.html`   登录页
- `static/app.js`          前端逻辑(数据刷新/图表/控制/主题/移动端)
- `static/style.css`       样式(暗色/浅色双主题, 移动端底部 Tab)
- `static/favicon.svg`     站点图标
- `static/manifest.json`   PWA 清单
- `static/vendor/`         第三方库(图表等)

> 路径约定:app.py 通过 `../frontend/templates` 与 `../frontend/static` 加载本目录,
> 移动/重命名请同步修改 `backend/app.py` 中 `template_dir` / `static_dir`。
