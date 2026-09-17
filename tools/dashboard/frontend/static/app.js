/* ============================================================
   BMS Dashboard 前端逻辑 (Chart.js 版本)
   对接 Flask + SocketIO 后端,匹配 bms_monitor (2).html 的 UI
   ============ 2026-08-22 配置开关修复批次(版本戳更新) ============
   1) 配置项改为开关样式(.switch/.slider, index.html + style.css)
   2) 开关点击兜底: .cfg-check-row 整行 + .switch 本体均内联 onclick 直接切换,
      不依赖 JS 事件绑定 —— 解决"开关点不动"的缓存/绑定类问题
   3) applyConfigGating 终版: 配置控件永不禁用, 权限由后端 role_required 兜底
   数据格式:
     - 电压: mV (后端) -> V (前端显示, /1000)
     - 电流: mA (后端) -> A (前端显示, /1000)
     - 温度: 0.1℃ (后端) -> ℃ (前端显示, /10)
     - 单体: 6 串 (cells 数组)
   ============================================================ */

// ============================================================
// 全局常量
// ============================================================
let NUM_CELLS = 6;                  // 电池串联数(动态: 由设备上报/参数自动更新)
let _numCellsApplied = 6;           // 当前已应用的串数(避免重复重建)
// 2026-08-09: 用户下发串数覆盖——用户下发后置为该值, handleBmsData 不再每帧
//   用设备上报串数(后端 cell_series_num=16)覆盖用户配置("适配一秒又回16"修复);
//   设备确认切换(上报 == 用户配置)后清除, 恢复由设备值驱动
let _userSeriesOverride = 0;
// 2026-08-09: 主曲线历史查询模式标志(6h/24h/7d)——
//   true 时 pushMainChart 暂停, 避免实时推送覆盖已渲染的历史数据
let _mainChartHistMode = false;
// 2026-08-09: 16路曲线历史范围切换防抖定时器(见 setCellVoltRange)
let _cellVoltRangeTimer = null;
// 2026-08-11: 主曲线历史范围切换防抖与请求中断(见 setMainChartRange / loadMainChartHistory)
let _mainChartRangeTimer = null;
let _mainChartHistoryAbort = null;
const MAX_CHART_POINTS = 30;        // 实时曲线保留 30 个点
const MAX_LOG_LINES = 200;          // 日志最大行数

/* 去重: 防止 SocketIO 推送 + /api/status 5s 轮询同一条
 * (id + timestamp + current) 组合重复则跳过可视化刷新 */
let _lastVisualKey = "";
let _curRangeCfg = null;

// ============================================================
// fetchWithRetry: 带重试的 fetch 封装(2026-09-07 新增)
//   网络抖动时自动重试, 不再静默失败; AbortError(用户中断)不重试
//   用法: fetchWithRetry("/api/status", opts, 3).then(r => r.json())...
// ============================================================
function fetchWithRetry(url, opts, retries) {
    retries = retries || 3;
    return fetch(url, opts).catch(function(err) {
        if (retries > 1 && err && err.name !== "AbortError") {
            return new Promise(function(resolve) { setTimeout(resolve, 1000); })
                .then(function() { return fetchWithRetry(url, opts, retries - 1); });
        }
        throw err;
    });
}

// 故障码 bitmask 定义(与 ESP32 端 bms_types.h 的 bms_fault_e 完全一致)
// Bug1 修复: 原映射表与固件定义错位, 导致故障描述完全错误
//   例如 0x80 实际是"放电过流保护", 原错误显示为"单体欠压保护"
//   0x20000 实际是"电量过低预警", 原错误显示为"任务死亡"
const FAULT_MAP = {
    /* ---- 电压类 ---- */
    0x00001: { level: "warn",  desc: "单体过压预警" },
    0x00002: { level: "error", desc: "单体过压保护" },
    0x00004: { level: "warn",  desc: "单体欠压预警" },
    0x00008: { level: "error", desc: "单体欠压保护" },
    0x02000: { level: "warn",  desc: "单体压差过大" },
    /* ---- 电流类 ---- */
    0x00010: { level: "warn",  desc: "充电过流预警" },
    0x00020: { level: "error", desc: "充电过流保护" },
    0x00040: { level: "warn",  desc: "放电过流预警" },
    0x00080: { level: "error", desc: "放电过流保护" },
    0x10000: { level: "warn",  desc: "持续过载预警" },
    /* ---- 温度类 ---- */
    0x00100: { level: "warn",  desc: "过温预警" },
    0x00200: { level: "error", desc: "过温保护" },
    0x04000: { level: "warn",  desc: "低温预警" },
    0x01000: { level: "error", desc: "低温保护" },
    0x08000: { level: "warn",  desc: "温升速率预警" },
    0x00400: { level: "fatal", desc: "热失控报警" },
    0x00800: { level: "fatal", desc: "短路" },
    /* ---- 状态/健康度 ---- */
    0x20000: { level: "warn",  desc: "电量过低预警" },
    0x40000: { level: "warn",  desc: "健康度衰减预警" },
    /* ---- 系统与通信 ---- */
    0x80000: { level: "warn",  desc: "通信/采样异常" },
    0x100000: { level: "fatal", desc: "电流采样失效" },
    /* 2026-08-09 新增: 绝缘检测(与固件 FAULT_INSULATION_WARN/PROT 对齐) */
    0x200000: { level: "warn",  desc: "绝缘电阻预警(<100Ω/V)" },
    0x400000: { level: "error", desc: "绝缘电阻保护(<50Ω/V, 切断主回路)" },
};

// 颜色常量(与 CSS 变量一致)
const COLOR = {
    // 2026-09-16 色板统一: 与 style.css :root 强调色令牌一一对应(此前 cyan/green/purple
    // 与主题令牌漂移, 同一语义色在卡片与图表里深浅不一)。日光模式经 --theme-rebuild
    // 重建图表时由 _themeVar() 动态取值, 主题切换即生效。
    get cyan()   { return _themeVar("--accent-cyan",   "#22d3ee"); },
    get blue()   { return _themeVar("--accent-blue",   "#3b82f6"); },
    get green()  { return _themeVar("--accent-green",  "#22c55e"); },
    get yellow() { return _themeVar("--accent-yellow", "#f59e0b"); },
    get red()    { return _themeVar("--accent-red",    "#ef4444"); },
    get purple() { return _themeVar("--accent-purple", "#a78bfa"); },
    textSec: "#64748b", textMut: "#475569",
    grid: "rgba(30,48,72,0.5)",     gridLight: "rgba(30,48,72,0.3)",
};
// 主题变量读取(COLOR getter 用): 取不到时回退暗色默认, 行为与 themeGridColor 一致
function _themeVar(name, fallback) {
    try {
        const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
        return v || fallback;
    } catch (e) { return fallback; }
}

/* ====== 2026-09-15 修复(可读性): 图表网格/刻度颜色随主题自适应 ======
 * 原实现 COLOR.grid/textMut 为硬编码暗色值, 切到日光模式后:
 * 刻度文字 #475569 在浅背景上偏灰难读, 网格线过淡几乎不可见.
 * 修复: 动态取 CSS 变量(--border-color/--text-muted), 主题切换即生效;
 * 加 --theme-rebuild 监听, 主题切换后自动刷新所有图表配色. */
function themeGridColor() {
    const v = getComputedStyle(document.documentElement).getPropertyValue("--border-color").trim();
    return v || COLOR.grid;
}
function themeMutedColor() {
    const v = getComputedStyle(document.documentElement).getPropertyValue("--text-muted").trim();
    return v || COLOR.textMut;
}
window.themeGridColor = themeGridColor;
window.themeMutedColor = themeMutedColor;

// ============================================================
// ViewRouter —— 企业级视图导航(单一职责 + 事件委托)
//   - 所有带 [data-view] 的元素(导航项、可点击卡片等)统一走委托,
//     不再散落大量内联 onclick=switchView(...), 便于维护与测试;
//   - 兼容鼠标点击与键盘 Enter/Space(无障碍);
//   - 与既有 switchView 解耦: 此处只负责"捕获意图 → 调用 switchView"。
// ============================================================
const ViewRouter = {
    /** 跳转到指定视图; 入参非法时静默忽略, 保证健壮性 */
    navigate(view) {
        if (typeof view !== "string" || view.trim() === "") return;
        if (typeof switchView === "function") switchView(view.trim());
    },
    /** 绑定全局委托监听(在 DOM 就绪后调用一次) */
    init() {
        // 点击委托: 命中最近的 [data-view] 祖先(支持按钮/卡片/链接任意结构)
        // 渐进增强: 已有内联 onclick 的元素(如侧边栏 <a>)由其自身处理, 委托只接管
        // 无内联处理的 [data-view](如 SOC-SOH 可点击卡片), 避免重复触发 switchView。
        // 2026-08-22 修复(#4 根因): 原生交互控件(含 file input)位于 data-view="settings"
        //   section 内, 原实现对其点击调 preventDefault → 阻止浏览器默认行为,
        //   固件发布"选择文件"对话框无法弹出(透明覆盖 input 也无效). 先跳过交互控件.
        document.body.addEventListener("click", (e) => {
            const t = e.target;
            if (t && t.closest && t.closest("input, select, textarea, button, label, a, .switch")) return;
            const el = t.closest("[data-view]");
            if (!el || el.hasAttribute("onclick")) return;
            e.preventDefault();
            this.navigate(el.getAttribute("data-view"));
        });
        // 键盘可达性: 聚焦在 [data-view] 上按 Enter/Space 等价于点击(同样跳过已有 onclick 的元素)
        document.body.addEventListener("keydown", (e) => {
            if (e.key !== "Enter" && e.key !== " ") return;
            const el = e.target.closest("[data-view]");
            if (!el || el.hasAttribute("onclick")) return;
            e.preventDefault();
            this.navigate(el.getAttribute("data-view"));
        });
    }
};

// 2026-08-10: 全局图表默认样式(对齐设计原型: 柔和字体/统一图例/平滑曲线/清爽交互)
if (window.Chart) {
    Chart.defaults.font.family = "'PingFang SC','Microsoft YaHei','Plus Jakarta Sans',system-ui,sans-serif";
    Chart.defaults.font.size = 11;
    Chart.defaults.color = COLOR.textSec;
    Chart.defaults.plugins.legend.labels.color = COLOR.textSec;
    Chart.defaults.plugins.legend.labels.usePointStyle = true;
    Chart.defaults.plugins.legend.labels.boxWidth = 8;
    Chart.defaults.plugins.legend.labels.padding = 12;
    Chart.defaults.elements.line.borderWidth = 2;
    Chart.defaults.elements.line.tension = 0.4;
    Chart.defaults.animation.duration = 350;
    Chart.defaults.animation.easing = "easeOutQuart";
    Chart.defaults.interaction.intersect = false;
    Chart.defaults.interaction.mode = "index";
}


// ============================================================
// 全局状态
// ============================================================
let socket = null;                  // SocketIO 客户端
let _wsPollFastMode = false;        // 2026-08-20(#6): WS 断开时置 true, 通信轮询加速到 5s
let _commPollTimer = null;          // 2026-08-20(#6): 通信状态轮询定时器句柄(自适应调速用)
let mainChart = null;               // 实时电压/温度曲线
let cellVoltChart = null;           // 16路单体电压曲线
let historyChart = null;            // 历史趋势图
let trendChart = null;              // SOC/SOH 30 天趋势
let selectedCellIndex = -1;         // 当前选中的单体索引, -1 = 全部(默认显示总电压)
let mainChartRange = "1min";        // 实时曲线时间范围: 1min/5min/1h
let cellVoltRange = "1min";         // 16路曲线时间范围: 1min/5min/1h/6h/24h/7d
let _cellVoltHistoryMode = false;   // 2026-08-08: true=历史模式(显示DB历史), false=实时滚动
// 2026-08-09: 16路曲线数据变化检测签名(无变化时跳过全量重绘, 提升性能)
let _cellVoltLastSig = "";
let historyRange = "1h";            // 历史范围: 1h/6h/24h/7d
let lastPayload = null;             // 最近一次收到的数据
let latencyMs = 0;                  // 通信延迟
let lastDataTs = 0;                 // 最近一次数据时间戳(用于计算延迟)

// 实时曲线数据缓冲
const chartLabels = [];
const chartVoltData = [];
const chartTempData = [];
const chartCurrData = [];    // 2026-08-08: 新增电流曲线(电压为0时仍有电流可见)
const chartOfflineData = [];   // 断线指示线: 离线时 push 最后有效电压值(灰色虚线), 正常时 push null
let lastValidVoltV = 0;        // 最后一个有效电压值(用于断线时保持水平虚线)
for (let i = 0; i < MAX_CHART_POINTS; i++) {
    chartLabels.push((MAX_CHART_POINTS - i) + "s");
    chartVoltData.push(null);
    chartTempData.push(null);
    chartCurrData.push(null);
    chartOfflineData.push(null);
}

// 单体数据缓存(供表格/CSV 使用)
let cellData = [];
function _rebuildCellData(n) {
    // 保留已有数据, 按新串数重建缓存
    const old = cellData || [];
    cellData = [];
    for (let i = 0; i < n; i++) {
        cellData.push(old[i] || { voltage: 0, temp: 0, soc: 0, status: "normal", irq: 0, deltaMv: 0 });
    }
}
_rebuildCellData(NUM_CELLS);

// 16路单体电压曲线数据缓冲(每条单体一条线, 时间窗与实时曲线一致)
const CELL_VOLT_MAX_POINTS = 30;
const cellVoltData = [];    // cellVoltData[i] = 第 i 路单体的历史电压数组(V)
const cellCurrData = [];    // 2026-08-18: 单体电压图上的电流曲线缓冲(A, 右轴 y1)
function _rebuildCellVoltData(n) {
    while (cellVoltData.length < n) {
        cellVoltData.push(new Array(CELL_VOLT_MAX_POINTS).fill(null));
    }
    while (cellVoltData.length > n) {
        cellVoltData.pop();
    }
    // 2026-08-18: 电流缓冲固定 30 点, 串数变化不影响
    if (cellCurrData.length === 0) {
        while (cellCurrData.length < CELL_VOLT_MAX_POINTS) cellCurrData.push(null);
    }
}
_rebuildCellVoltData(NUM_CELLS);

// 16 路曲线配色(HSL 色环均匀分布, 深色背景可读)
const CELL_VOLT_COLORS = [
    "#22d3ee", "#a78bfa", "#f472b6", "#34d399", "#fbbf24",
    "#60a5fa", "#fb7185", "#4ade80", "#c084fc", "#facc15",
    "#2dd4bf", "#f97316", "#818cf8", "#f43f5e", "#38bdf8", "#e879f9",
];
function _cellVoltColor(i) {
    return CELL_VOLT_COLORS[i % CELL_VOLT_COLORS.length];
}

// ============================================================
// 16路单体电压曲线(全部单体同屏对比, 支持 6S~16S 动态串数)
// 2026-08-08: try-catch 保护 - 单个图表初始化失败不影响其他图表
// ============================================================
function initCellVoltChart() {
    try {
        const ctx = $("cellVoltChart");
        if (!ctx) return;
        const c = ctx.getContext("2d");
        const datasets = [];
        for (let i = 0; i < NUM_CELLS; i++) {
            datasets.push({
                label: "#" + (i + 1),
                data: cellVoltData[i] ? cellVoltData[i].slice() : [],
                borderColor: _cellVoltColor(i),
                borderWidth: 1.2,
                tension: 0.3,
                pointRadius: 0,
                yAxisID: "y",
            });
        }
        // 2026-08-18: 单体电压图追加电流曲线(右轴 y1), 便于对照"充放电时单体压差变化"
        if (cellCurrData.length === 0) {
            while (cellCurrData.length < CELL_VOLT_MAX_POINTS) cellCurrData.push(null);
        }
        datasets.push({
            label: "电流 (A)",
            data: cellCurrData.slice(),
            borderColor: COLOR.green,
            borderWidth: 2,
            borderDash: [5, 3],
            tension: 0.3,
            pointRadius: 0,
            yAxisID: "y1",
            spanGaps: false,
        });
        cellVoltChart = new Chart(c, {
            type: "line",
            data: { labels: [], datasets: datasets },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                animation: { duration: 150 },
                interaction: { mode: "index", intersect: false },
                plugins: {
                    // ===== 2026-09-07 Chart.js decimation 降采样(大数据量渲染优化) =====
                    //   7天数据可能有上万点, min-max 算法按像素密度保留极值, 渲染提速 5~10 倍
                    decimation: { enabled: true, algorithm: "min-max", samples: 200 },
                    legend: {
                        labels: { color: COLOR.textSec, font: { size: 10 }, boxWidth: 10, padding: 8, usePointStyle: true },
                    },
                    // ===== 2026-08-10 修复: 16路曲线悬停显示各单体电压 tooltip =====
                    tooltip: {
                        enabled: true,
                        mode: "index",
                        intersect: false,
                        backgroundColor: "rgba(15,23,42,0.92)",
                        titleColor: "#e2e8f0",
                        bodyColor: "#cbd5e1",
                        borderColor: "rgba(100,116,139,0.45)",
                        borderWidth: 1,
                        padding: 6,
                        cornerRadius: 6,
                        displayColors: true,
                        callbacks: {
                            label: (item) => {
                                const v = item.raw;
                                if (v === null || v === undefined || v === "") return item.dataset.label + ": --";
                                return item.dataset.label + ": " + Number(v).toFixed(3) + "V";
                            },
                        },
                    },
                },
                scales: {
                    x: {
                        grid: { color: themeGridColor() },
                        ticks: { color: themeMutedColor(), font: { size: 10 }, maxTicksLimit: 8, autoSkip: true, autoSkipPadding: 16,
                                 maxRotation: 0, minRotation: 0,
                                 // 2026-08-10 坐标优化: 与主曲线一致, 长范围去秒防重叠
                                 callback: function(value) {
                                     let lb = "";
                                     try { lb = this.getLabelForValue(value) || ""; } catch (e) {}
                                     if (!lb) return lb;
                                     const m = /^(\d{2}:\d{2}):\d{2}$/.exec(lb);
                                     if (m && (cellVoltRange === "5min" || cellVoltRange === "1h" || cellVoltRange === "6h")) {
                                         return m[1];
                                     }
                                     return lb;
                                 },
                        },
                    },
                    y: {
                        position: "left",
                        grid: { color: themeGridColor() },
                        ticks: { color: COLOR.cyan, font: { size: 10 } },
                        title: { display: true, text: "电压 (V)", color: COLOR.cyan, font: { size: 10 } },
                        suggestedMin: 0,
                        suggestedMax: 5,
                    },
                    y1: {
                        position: "right",
                        grid: { drawOnChartArea: false },
                        ticks: { color: COLOR.green, font: { size: 10 } },
                        title: { display: true, text: "电流 (A)", color: COLOR.green, font: { size: 10 } },
                        suggestedMin: -10,
                        suggestedMax: 10,
                    },
                },
            },
        });
        // 初始 X 轴标签(与实时曲线一致: 最右=now)
        _regenCellVoltLabels();
        if (cellVoltChart) {
            cellVoltChart.data.labels = chartLabels.slice();
            cellVoltChart.update("none");
        }
    } catch (e) {
        console.error("[cellVoltChart] 初始化失败:", e);
        cellVoltChart = null;   // 标记失败, pushCellVoltChart 会自动跳过
    }
}

// 重新生成 16 路曲线 X 轴标签(复用实时曲线的标签)
// 2026-08-10 P3: 原为空实现, chartLabels 由 _regenChartLabels 维护,
//   此函数已无实际作用, 保留为空壳以兼容历史调用点
function _regenCellVoltLabels() {
    // chartLabels 由 _regenChartLabels 维护, 直接引用即可
}

// 16路曲线时间范围切换(1分钟/5分钟)
function setCellVoltRange(range, el) {
    cellVoltRange = range;
    if (el) {
        const container = el.closest(".chart-actions") || el.parentElement;
        container.querySelectorAll(".chart-tab").forEach(b => b.classList.remove("active"));
        el.classList.add("active");
    }
    // 2026-08-09 性能优化: 历史范围切换防抖(300ms)——快速点击 1h/6h/24h/7d
    //   时合并重复请求, 避免瞬时多次 fetch /api/history 拖慢后端/页面
    // 2026-08-08: 1h/6h/24h/7d = 历史模式(拉取 DB 历史单体电压);
    //             1min/5min = 实时模式(滚动缓冲)
    const modeTag = $("cellVoltModeTag");
    if (range === "1h" || range === "6h" || range === "24h" || range === "7d") {
        if (_cellVoltRangeTimer) clearTimeout(_cellVoltRangeTimer);
        _cellVoltRangeTimer = setTimeout(() => {
            _cellVoltRangeTimer = null;
            _cellVoltHistoryMode = true;
            if (modeTag) { modeTag.textContent = "历史"; modeTag.style.color = "var(--accent-yellow)"; }
            // 2026-08-11: 用户点击历史范围 → 立即加载态(感知速度)
            _chartSetLoading(cellVoltChart, true);
            loadCellVoltHistory(range, true);
            addLog("[历史] 16路曲线切换历史范围: " + range, "info");
        }, 300);
        return;
    }
    // 实时模式: 恢复滚动缓冲(清空历史数据, 从最新帧重新累积)
    _cellVoltHistoryMode = false;
    if (modeTag) { modeTag.textContent = "实时"; modeTag.style.color = "var(--accent-cyan)"; }
    const emptyHint = $("cellVoltEmptyHint");
    if (emptyHint) emptyHint.style.display = "none";
    // 与实时曲线同点数: 1min=30点, 5min=150点
    const points = range === "5min" ? 150 : 30;
    for (let i = 0; i < cellVoltData.length; i++) {
        const arr = cellVoltData[i];
        while (arr.length < points) arr.unshift(null);
        while (arr.length > points) arr.shift();
    }
    if (cellVoltChart) cellVoltChart.update("none");
    addLog("[视图] 16路曲线范围切换: " + (range === "5min" ? "5 分钟" : "1 分钟"), "info");
}

// ============================================================
// 16路单体电压历史曲线加载(1h/6h/24h/7d)
// 从 /api/history 拉取记录, 按 recv_time 绘制每路单体电压
// ============================================================
function loadCellVoltHistory(range, showLoading) {
    const minMap = { "1h": 60, "6h": 360, "24h": 1440, "7d": 10080 };
    const minutes = minMap[range] || 60;
    const limit = range === "7d" ? 100000 : (range === "24h" ? 50000 : 3000);
    // 2026-09-07: sessionStorage 缓存 — 缓存上次 API 响应, 页面刷新后秒开
    const _cacheKey = "cellVolt_" + range;
    let _useCache = false;
    let _cachedRes = null;
    try {
        const cached = sessionStorage.getItem(_cacheKey);
        if (cached) {
            const d = JSON.parse(cached);
            if (d && d.ts && (Date.now() - d.ts < (minutes * 60000) / 2) && d.data) {
                _cachedRes = d.data;
                _useCache = true;
            }
        }
    } catch (e) {}
    if (showLoading) _chartSetLoading(cellVoltChart, true);
    // 2026-08-09 容错: 接口超时(15s)用 AbortController 中断, 显示占位提示而非空白
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(), 15000);
    // 2026-09-07: 缓存命中且新鲜时跳过 fetch(页面刷新秒开)
    if (_useCache && cellVoltChart) {
        clearTimeout(timer);
        try {
            // 用缓存数据直接渲染(最小路径: 跳过聚合, 直接用 res.agg 或空)
            const _res = _cachedRes;
            const _rows = (_res && Array.isArray(_res.data)) ? _res.data : [];
            if (_rows.length > 0) {
                let _mc = 0;
                _rows.forEach(row => { const c = (row.cells || []).length; if (c > _mc) _mc = c; });
                if (_mc > 0) {
                    const _agg = (_res && _res.agg) ? _res.agg : _aggregateHistoryRows(_rows, range);
                    // 重建 datasets
                    cellVoltChart.data.datasets = [];
                    for (let i = 0; i < _mc; i++) {
                        cellVoltChart.data.datasets.push({ label: "#" + (i+1), data: [],
                            borderColor: _cellVoltColor(i), borderWidth: 1.2, tension: 0.3, pointRadius: 0, yAxisID: "y" });
                    }
                    cellVoltChart.data.datasets.push({ label: "无数据区段", data: [], borderColor: "#8a8f98",
                        borderWidth: 1, borderDash: [6,4], tension: 0, pointRadius: 0, yAxisID: "y", spanGaps: true });
                    const _perCell = Array.from({ length: _mc }, (_, ci) => _agg.cells.map(c => (c && ci < c.length) ? c[ci] : null));
                    cellVoltChart.data.labels = _agg.labels;
                    for (let i = 0; i < _mc; i++) cellVoltChart.data.datasets[i].data = _perCell[i];
                    cellVoltChart.data.datasets[cellVoltChart.data.datasets.length - 1].data = _agg.off;
                    _applyDayMarkers(cellVoltChart, _agg.dayMarkers);
                    cellVoltChart.update("none");
                }
            }
            addLog("[历史] 16路曲线缓存命中 " + range, "log-info");
        } catch (e) { /* 缓存渲染失败, 继续走网络 */ }
    }
    fetch("/api/history?minutes=" + minutes + "&limit=" + limit + "&agg=1", { signal: ac.signal })
        .then(r => r.json())
        .then(res => {
            clearTimeout(timer);
            if (!cellVoltChart) return;
            /* 2026-09-15 修复(转圈圈): agg=1 时后端只回 {agg:...} 而无 data 字段,
             * 原实现 rows=[] → maxCells=0 → 误走"无数据"分支提前 return,
             * agg 数据被丢弃, 图表既无线条又残留加载态 → 用户看到"一直转圈圈".
             * 修复: agg 优先——有 agg 则直接取聚合结果与路数, 无 agg 才回退原始行. */
            const _agg0 = (res && res.agg) ? res.agg : null;
            const rows = (res && Array.isArray(res.data)) ? res.data : [];
            // 找出历史数据中出现的最大串数(以 cells 长度为准)
            let maxCells = 0;
            if (_agg0 && Array.isArray(_agg0.cells) && _agg0.cells.length > 0) {
                // agg 模式: cells 为 [点][路], 路数取任一点的长度
                for (const c of _agg0.cells) { if (c && c.length > maxCells) maxCells = c.length; }
            } else {
                rows.forEach(row => {
                    const c = (row.cells || []).length;
                    if (c > maxCells) maxCells = c;
                });
            }
            const emptyHint = $("cellVoltEmptyHint");
            if (rows.length === 0 && maxCells === 0) {
                if (emptyHint) {
                    emptyHint.textContent = "该时间段无单体电压历史数据(设备未上报/未接线)";
                    emptyHint.style.display = "block";
                }
                cellVoltChart.data.labels = [];
                cellVoltChart.data.datasets.forEach(ds => { ds.data = []; });
                cellVoltChart.update("none");
                if (showLoading) _chartSetLoading(cellVoltChart, false);
                return;
            }
            // 2026-08-09 修复: 历史单体曲线**不再调用 applySeriesNum**——
            //   原 `if (maxCells !== NUM_CELLS) applySeriesNum(maxCells)` 会把
            //   历史数据最大串数写进全局 NUM_CELLS, 与 handleBmsData 的实时配置串数
            //   (cell_series_num)互相覆盖, 导致日志 "8↔16" 反复循环、用户下发不生效.
            //   历史曲线只按自身 maxCells 渲染 datasets(下方循环), 不触碰全局串数;
            //   全局串数唯一来源 = 后端实时配置 cell_series_num(handleBmsData 3946 行).
            // 串数自适应(历史最高串数) —— 仅用于本图 datasets 路数, 见下方 for 循环
            const histCells = maxCells;
            // 重建 datasets(每路一条线 + 无数据区段灰色虚线)
            cellVoltChart.data.datasets = [];
            for (let i = 0; i < maxCells; i++) {
                cellVoltChart.data.datasets.push({
                    label: "#" + (i + 1),
                    data: [],
                    borderColor: _cellVoltColor(i),
                    borderWidth: 1.2,
                    tension: 0.3,
                    pointRadius: 0,
                    yAxisID: "y",
                });
            }
            // 2026-08-08: 无数据区段指示线(灰色虚线, 显示设备离线/断传的时间段)
            cellVoltChart.data.datasets.push({
                label: "无数据区段",
                data: [],
                borderColor: "#8a8f98",
                borderWidth: 1,
                borderDash: [6, 4],
                tension: 0,
                pointRadius: 0,
                yAxisID: "y",
                spanGaps: true,
            });
            // ===== 2026-08-10: 按范围单位聚合平均(替代逐条绘制) =====
            //   与主曲线一致: 1h=60点/分钟, 6h=36点/10分钟, 24h=24点/小时, 7d=168点/小时;
            //   agg.cells 为 [点][路] 结构, 转置为 [路][点] 供 datasets 使用
            const agg = (res && res.agg) ? res.agg : _aggregateHistoryRows(rows, range);
            const labels = agg.labels;
            const perCell = Array.from({ length: maxCells }, (_, ci) =>
                agg.cells.map(c => (c && ci < c.length) ? c[ci] : null)
            );
            cellVoltChart.data.labels = labels;
            for (let i = 0; i < maxCells; i++) {
                cellVoltChart.data.datasets[i].data = perCell[i];
            }
            // 无数据区段检测(聚合结果自带): 相邻桶间隔 > 桶长视为断传, 用上一桶有效电压画水平虚线
            cellVoltChart.data.datasets[cellVoltChart.data.datasets.length - 1].data = agg.off;
            // X 轴刻度上限随范围切换(7d 按天稀疏, 小时范围按分钟)
            if (cellVoltChart.options && cellVoltChart.options.scales && cellVoltChart.options.scales.x && cellVoltChart.options.scales.x.ticks) {
                cellVoltChart.options.scales.x.ticks.maxTicksLimit = HIST_MAXTICKS[range] || 12;
            }
            // 动态 Y 轴
            const allV = [].concat.apply([], perCell).filter(v => v !== null && !isNaN(v) && v > 0);
            if (allV.length > 0) {
                const vMin = Math.min.apply(null, allV);
                const vMax = Math.max.apply(null, allV);
                const vPad = Math.max(0.1, (vMax - vMin) * 0.15);
                cellVoltChart.options.scales.y.min = Math.max(0, vMin - vPad);
                cellVoltChart.options.scales.y.max = vMax + vPad;
            }
            // 全 0 提示(2026-08-09: 改用图表组件层封装)
            const anyValid = allV.length > 0;
            ChartComponent.showHint(anyValid ? "" : ChartUtil.hintText("no_data"), !anyValid);
            // 2026-08-11 跨天统计日期: 直接使用聚合结果返回的跨天点, 7d 标签全带 MM-DD 后避免误判
            _applyDayMarkers(cellVoltChart, agg.dayMarkers);
            cellVoltChart.update("none");
            if (showLoading) _chartSetLoading(cellVoltChart, false);
            addLog("[历史] 16路曲线加载 " + rows.length + " 条 (" + range + ")", "log-success");
            // 2026-09-07: 缓存写入(页面刷新秒开)
            try { sessionStorage.setItem("cellVolt_" + range, JSON.stringify({ts: Date.now(), data: res})); } catch (e) {}
        })
        .catch(err => {
            clearTimeout(timer);
            if (showLoading) _chartSetLoading(cellVoltChart, false);
            // 2026-08-09 容错: 接口超时/异常时显示占位提示, 不让图表空白
            const emptyHint = $("cellVoltEmptyHint");
            if (emptyHint) {
                emptyHint.textContent = (err && err.name === "AbortError")
                    ? "历史数据请求超时(15s), 请检查后端/网络后重试"
                    : "历史数据加载失败: " + (err && err.message ? err.message : String(err));
                emptyHint.style.display = "block";
            }
            addLog("[历史] 16路曲线加载失败: " + (err && err.message ? err.message : err), "log-error");
        });
}

// 推送一帧数据到 16 路曲线(p 来自 bms_data / api_status)
function pushCellVoltChart(p) {
    if (!cellVoltChart || !p) return;
    // 2026-08-08: 历史模式下实时推送不覆盖历史曲线(切回实时时再恢复滚动)
    if (_cellVoltHistoryMode) return;
    // no_data: 全部 push null 形成断线
    if (p.no_data) {
        for (let i = 0; i < cellVoltData.length; i++) {
            cellVoltData[i].shift();
            cellVoltData[i].push(null);
        }
        if (cellCurrData.length) { cellCurrData.shift(); cellCurrData.push(null); }
        cellVoltChart.data.labels = chartLabels.slice();
        for (let i = 0; i < cellVoltChart.data.datasets.length; i++) {
            // 2026-08-18: 最后一条为电流曲线, 用 cellCurrData 填充
            if (i === cellVoltChart.data.datasets.length - 1) {
                cellVoltChart.data.datasets[i].data = cellCurrData.slice();
            } else {
                cellVoltChart.data.datasets[i].data = (cellVoltData[i] || []).slice();
            }
        }
        cellVoltChart.update("none");
        return;
    }
    const cells = p.cells || [];
    // 2026-08-09 性能优化: 数据变化检测——cells 与上次相同时跳过全量 update,
    //   避免每帧 bms_data 都触发 32 路 datasets 全量重绘(主线程被占用 → 曲线/按键响应慢)
    // 2026-08-09 修复: 全 0/无有效数据时**不跳过**(hasData=false)——否则 BQ76952 未接线时
    //   cells 恒为 "0,0,0,0,0,0"(sig 不变)会永远 return, 导致实时曲线不重绘、X 轴时间不滚动
    const sig = cells.join(",");
    const hasData = cells.some(v => Number(v || 0) > 0);
    // 2026-08-21 修复(#3 曲线数量与串数不一致): 数据变化跳过的判断改为与
    //   配置串数 NUM_CELLS 对齐——原条件用 cells.length(设备上报格数), 上报
    //   格数 < 配置串数时数据集长度永远不等于 cells.length, 跳过优化失效.
    if (hasData && sig === _cellVoltLastSig && cellVoltData.length === NUM_CELLS) {
        return;   // 有数据且无变化, 跳过重绘
    }
    _cellVoltLastSig = sig;
    // 串数变化时重建缓冲(动态适配 6S~16S)
    // 2026-08-21 修复(#3): 重建数量以用户配置 NUM_CELLS 为准(与顶部串数/3D 视图一致),
    //   原实现用 cells.length(设备上报格数), 设备上报少于配置串数时曲线数量与串数对不上.
    if (NUM_CELLS >= 1 && NUM_CELLS !== cellVoltData.length) {
        _rebuildCellVoltData(NUM_CELLS);
        // 同步图表 datasets(保留配色)
        cellVoltChart.data.datasets = [];
        for (let i = 0; i < cellVoltData.length; i++) {
            cellVoltChart.data.datasets.push({
                label: "#" + (i + 1),
                data: cellVoltData[i].slice(),
                borderColor: _cellVoltColor(i),
                borderWidth: 1.2,
                tension: 0.3,
                pointRadius: 0,
                yAxisID: "y",
            });
        }
        // 2026-08-18: 串数重建后追回电流曲线(末条, 右轴 y1)
        cellVoltChart.data.datasets.push({
            label: "电流 (A)",
            data: cellCurrData.slice(),
            borderColor: COLOR.green,
            borderWidth: 2,
            borderDash: [5, 3],
            tension: 0.3,
            pointRadius: 0,
            yAxisID: "y1",
            spanGaps: false,
        });
    }
    for (let i = 0; i < cellVoltData.length; i++) {
        const v = (i < cells.length) ? (Number(cells[i] || 0) / 1000) : null;
        cellVoltData[i].shift();
        cellVoltData[i].push(v);
    }
    // 2026-08-18: 电流曲线同步滚动(末条)
    if (cellCurrData.length) {
        cellCurrData.shift();
        cellCurrData.push(Number(p.current || 0) / 1000);
    }
    cellVoltChart.data.labels = chartLabels.slice();
    for (let i = 0; i < cellVoltChart.data.datasets.length; i++) {
        // 2026-08-18: 末条为电流曲线(y1), 用 cellCurrData 填充; 其余为单体电压
        if (i === cellVoltChart.data.datasets.length - 1) {
            cellVoltChart.data.datasets[i].data = cellCurrData.slice();
        } else {
            cellVoltChart.data.datasets[i].data = (cellVoltData[i] || []).slice();
        }
    }
    // 动态 Y 轴(有效单体电压范围, 空数据保护)
    const validV = [];
    for (let i = 0; i < cellVoltData.length; i++) {
        const last = cellVoltData[i][cellVoltData[i].length - 1];
        if (last !== null && !isNaN(last) && last > 0) validV.push(last);
    }
    if (validV.length > 0) {
        const vMin = Math.min(...validV);
        const vMax = Math.max(...validV);
        const vPad = Math.max(0.1, (vMax - vMin) * 0.15);
        cellVoltChart.options.scales.y.min = Math.max(0, vMin - vPad);
        cellVoltChart.options.scales.y.max = vMax + vPad;
    }
    // 2026-08-09: 全部单体电压为 0 时给出提示(常见于 BQ76952 未接线/未启用)
    //   改用图表组件层封装(ChartComponent.showHint + ChartUtil)
    if (ChartUtil.isAllZeroCells(cells)) {
        ChartComponent.showHint(ChartUtil.hintText("no_data"), true);
    } else {
        ChartComponent.showHint("", false);
    }
    cellVoltChart.update("none");
}

// ============================================================
// 串数动态适配: 根据设备上报/后端参数更新 NUM_CELLS 并重建 UI
// ============================================================
function applySeriesNum(n) {
    n = parseInt(n, 10);
    // 2026-08-09 修复: 上限 16→32(与固件 BMS_MAX_CELL_SERIES_NUM=32 一致, 原 16 会拒绝 17~32 串)
    if (!(n >= 1 && n <= 32)) return;   // 非法串数忽略(设备端上限 BMS_MAX_CELL_SERIES_NUM=32)
    // 2026-08-19 修复: 顶部"串数"显示独立于重建——即使串数 == 初始假设值(常见为 6 串)也需填充,
    //   否则首次加载 topSeriesNum 永远保持占位 '--'(原 return 提前导致不显示)
    const ts = $("topSeriesNum");
    if (ts) ts.textContent = n;
    if (n === _numCellsApplied) return;
    const old = NUM_CELLS;
    _numCellsApplied = n;
    NUM_CELLS = n;
    _rebuildCellData(n);
    // 重建单体选择器按钮(#1~#N)
    if (typeof buildCellSelector === "function") buildCellSelector();
    // 当前选中的单体超出范围时回退到"全部"
    if (selectedCellIndex >= n) {
        selectedCellIndex = -1;
        if (typeof setChartCell === "function") setChartCell(-1);
    }
    // 同步电池参数面板的"串联节数"输入框
    const bs = $("batSeries");
    if (bs) bs.value = n;
    // 重新渲染表格 + 移动端可视化
    if (lastPayload) {
        if (typeof renderCellTable === "function") renderCellTable(lastPayload.cells);
    } else {
        if (typeof renderCellTable === "function") renderCellTable([]);
    }
    if (typeof updateMobileViz === "function") updateMobileViz();
    // 2026-08-09 修复: 串数联动补全——3D 电池组视图/算法可视化中心随串数同步重建
    //   (原逻辑仅表格/选择器/顶部串数联动, 导致"设置 N 路只有电芯数据变, 其他没反应")
    if (typeof renderBattery3D === "function") renderBattery3D(lastPayload ? lastPayload.cells : []);
    // 2026-08-11: 串数变化后重置实时渲染签名, 避免被节流门控误吞首次重绘
    _last3DSig = (lastPayload && lastPayload.cells) ? lastPayload.cells.join(",") : " ";
    _lastCellTableSig = _last3DSig;
    // 2026-08-09 修复: 16 路单体曲线通道随串数重建——
    //   原通道增减仅依赖 pushCellVoltChart 帧检测(历史模式 _cellVoltHistoryMode 下直接
    //   return 不触发), 导致"下发 N 串后单体电压曲线数量不变". 此处主动重建缓冲与 datasets.
    if (typeof _rebuildCellVoltData === "function") _rebuildCellVoltData(n);
    if (cellVoltChart && typeof _cellVoltColor === "function") {
        cellVoltChart.data.datasets = [];
        for (let i = 0; i < cellVoltData.length; i++) {
            cellVoltChart.data.datasets.push({
                label: "#" + (i + 1),
                data: cellVoltData[i].slice(),
                borderColor: _cellVoltColor(i),
                borderWidth: 1.2,
                tension: 0.3,
                pointRadius: 0,
                yAxisID: "y",
            });
        }
        cellVoltChart.update("none");
    }
    addLog("[串数] 已动态适配为 " + n + " 串 (原 " + old + " 串)", "log-success");
}

// 当前告警列表(用于 ack/dismiss)
let currentAlerts = [];
let alertIdSeq = 0;

// 操作日志(本地显示缓存 + 后端持久化队列)
let operationLog = [];
let logCounter = 0;
const LOG_ICON = { info: "🔵", warn: "🟡", error: "🔴", success: "🟢" };
let _opLogQueue = [];
let _opLogFlushTimer = null;
const OP_LOG_FLUSH_MS = 5000;
let _opLogRangeInited = false;

// 控制开关状态
const switchState = { MainRelay: true, ChgMOS: true, DischgMOS: true, Balance: false };
let balanceMode = "off";            // off / passive / active

// ============================================================
// 工具函数
// ============================================================
function $(id) { return document.getElementById(id); }

// Bug2 修复: 左侧图标点击跳转到对应区域
function scrollToSection(sectionId) {
    // 更新侧边栏高亮
    document.querySelectorAll(".sidebar-item").forEach(b => b.classList.remove("active"));
    if (event && event.currentTarget) event.currentTarget.classList.add("active");

    if (sectionId === "top") {
        // 滚动到顶部
        const cs = $("contentScroll");
        if (cs) cs.scrollTo({ top: 0, behavior: "smooth" });
        else window.scrollTo({ top: 0, behavior: "smooth" });
        return;
    }
    const target = $(sectionId);
    if (target) {
        target.scrollIntoView({ behavior: "smooth", block: "start" });
        // 闪烁高亮效果
        target.style.transition = "background 0.3s";
        const origBg = target.style.background;
        target.style.background = "rgba(6,182,212,0.15)";
        setTimeout(() => { target.style.background = origBg; }, 1000);
    }
}

function fmtNow() {
    return new Date().toLocaleTimeString("zh-CN", { hour12: false });
}

// ============================================================
// 2026-08-10 F4: HTML 转义工具 —— 所有 innerHTML 拼接设备/云端
//   可控字段(设备名/告警描述/日志文本/报表名称)必须经此转义,
//   防存储型 XSS(恶意 SSID/设备名/告警文本注入脚本)
// ============================================================
function htmlEsc(v) {
    return String(v == null ? "" : v)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#39;");
}

function fmtTime(ts) {
    if (!ts) return "--";
    // 后端 timestamp 是毫秒, recv_time 是秒
    let ms = ts;
    if (ms < 1e12) ms = ms * 1000;
    return new Date(ms).toLocaleTimeString("zh-CN", { hour12: false });
}

function fmtDateTime(ts) {
    if (!ts) return "--";
    let ms = ts;
    if (ms < 1e12) ms = ms * 1000;
    const d = new Date(ms);
    return d.toLocaleDateString("zh-CN") + " " + d.toLocaleTimeString("zh-CN", { hour12: false });
}

/* ====== 2026-08-09: 曲线横轴智能时间标签 ======
 * 需求: 7天/24小时等跨天/跨月数据时, 横轴必须能看出是哪一天/哪一月
 * 实现: 每条数据先计算"日期键"(YYYY-MM-DD), 与上一条比较:
 *   - 同一天内        -> 只显示 HH:MM:SS
 *   - 跨天(日期键变)  -> 显示 "MM-DD HH:MM" (跨月自然带出月份, 跨年带出年份)
 * 例: 同一天 "23:59:15", 次日第一条 "08-10 00:00", 跨月 "08-01 00:00"
 * @param d        当前时间 Date
 * @param prevKey  上一条的日期键(YYYY-MM-DD), 首条传 null
 * @return {label, key} 横轴标签与当前日期键 */
function fmtAxisSmartTime(d, prevKey) {
    const hh = String(d.getHours()).padStart(2, "0");
    const mm = String(d.getMinutes()).padStart(2, "0");
    const ss = String(d.getSeconds()).padStart(2, "0");
    const y = d.getFullYear();
    const mo = String(d.getMonth() + 1).padStart(2, "0");
    const da = String(d.getDate()).padStart(2, "0");
    const key = y + "-" + mo + "-" + da;
    if (prevKey !== null && prevKey !== key) {
        // 跨天/跨月: 加日期前缀(月份不同自然显示 MM-DD), 并标记跨天(供日期分隔线绘制)
        return { label: mo + "-" + da + " " + hh + ":" + mm, key: key, dayChanged: true, day: mo + "-" + da };
    }
    return { label: hh + ":" + mm + ":" + ss, key: key, dayChanged: false, day: null };
}

/* ====== 2026-08-11: 历史曲线按范围单位聚合平均(覆盖完整时间轴) ======
 * 需求: 7天/24小时等长范围下上千条原始记录挤成一团, X轴单位不随范围切换;
 *       用户进一步反馈"坐标没有匹配好"——即 X 轴应始终铺满所选范围, 无数据时
 *       留空而不是只画有数据的点。
 * 实现: 按范围选聚合桶(1h=1min / 6h=10min / 24h=1h / 7d=1h),
 *       从"现在"往前铺满整个范围生成固定桶(1h=60点/6h=36点/24h=24点/7d=168点);
 *       桶内有数据取平均, 无数据填 null 让曲线断开; 标签随范围切换单位,
 *       7d 统一显示 "MM-DD HH:00", 跨天点自动返回 dayMarkers 供日期分隔线使用. */
const HIST_BUCKET = { "1h": 60, "6h": 600, "24h": 900, "7d": 1800 };   // 聚合桶大小(秒) 2026-08-20: 24h/7d 加细(原 3600)
const HIST_RANGE_SEC = { "1h": 3600, "6h": 21600, "24h": 86400, "7d": 604800 };
const HIST_MAXTICKS = { "1h": 12, "6h": 12, "24h": 16, "7d": 12 };      // X轴刻度上限(2026-08-20: 随加细桶提高)

// 2026-08-11: 曲线加载态切换 —— 按下范围按钮后给 canvas 父容器加 .chart-loading,
//   CSS 显示居中旋转指示 + 曲线淡出, 让"曲线生成中"即时可见(感知速度).
function _chartSetLoading(chart, on) {
    try {
        if (chart && chart.canvas && chart.canvas.parentElement) {
            chart.canvas.parentElement.classList.toggle("chart-loading", !!on);
        }
    } catch (e) {}
}

function _aggregateHistoryRows(rows, range) {
    const bucket = HIST_BUCKET[range] || 60;
    const rangeSec = HIST_RANGE_SEC[range] || 3600;
    const endTs = Math.floor(Date.now() / 1000);
    const startTs = Math.floor((endTs - rangeSec) / bucket) * bucket;
    const endBucket = Math.floor(endTs / bucket) * bucket;

    // 统计最大串数(所有行, 不只是范围内)
    let cellCount = 0;
    (rows || []).forEach(r => {
        const c = (r.cells || []).length;
        if (c > cellCount) cellCount = c;
    });

    // 按桶聚合数据(只纳入选中时间范围内的点)
    const bucketMap = {};
    (rows || []).forEach(row => {
        const ts = Number(row.recv_time || 0);
        if (!ts) return;
        const b = Math.floor(ts / bucket) * bucket;
        if (b < startTs || b > endBucket) return;
        if (!bucketMap[b]) {
            bucketMap[b] = { sumV: 0, vCnt: 0, sumT: 0, tCnt: 0, sumC: 0, cCnt: 0, cellSum: Array(cellCount).fill(0), cellN: Array(cellCount).fill(0) };
        }
        const agg = bucketMap[b];
        const pv = Number(row.pack_v || 0) / 1000;
        if (pv > 0) { agg.sumV += pv; agg.vCnt++; }
        const tm = Number(row.temp_max || 0) / 10;
        if (tm > 0) { agg.sumT += tm; agg.tCnt++; }
        agg.sumC += Number(row.current || 0) / 1000;
        agg.cCnt++;
        const cells = row.cells || [];
        for (let i = 0; i < cellCount; i++) {
            const cv = i < cells.length ? (Number(cells[i] || 0) / 1000) : 0;
            if (cv > 0) { agg.cellSum[i] += cv; agg.cellN[i]++; }
        }
    });

    const out = { labels: [], volt: [], temp: [], curr: [], cells: [], bucketTs: [], off: [], dayMarkers: [] };
    let prevDay = null, bucketIdx = 0;
    const totalBuckets = Math.max(1, Math.floor((endBucket - startTs) / bucket) + 1);
    for (let b = startTs; b <= endBucket; b += bucket, bucketIdx++) {
        const d = new Date(b * 1000);
        const pad = n => String(n).padStart(2, "0");
        const hh = pad(d.getHours()), mm = pad(d.getMinutes());
        const mo = pad(d.getMonth() + 1), da = pad(d.getDate());
        const day = mo + "-" + da;
        // 标签按范围单位: 7d 统一显示 MM-DD HH:00, 其余显示 HH:MM, 跨天加 MM-DD 前缀
        let label;
        if (range === "7d") label = day + " " + hh + ":00";
        else label = (prevDay !== null && day !== prevDay) ? (day + " " + hh + ":" + mm) : (hh + ":" + mm);
        out.labels.push(label);
        out.bucketTs.push(b);
        const agg = bucketMap[b];
        out.volt.push(agg && agg.vCnt > 0 ? Number((agg.sumV / agg.vCnt).toFixed(2)) : null);
        out.temp.push(agg && agg.tCnt > 0 ? Number((agg.sumT / agg.tCnt).toFixed(1)) : null);
        out.curr.push(agg && agg.cCnt > 0 ? Number((agg.sumC / agg.cCnt).toFixed(2)) : null);
        const cells = [];
        for (let i = 0; i < cellCount; i++) {
            cells.push(agg && agg.cellN[i] > 0 ? Number((agg.cellSum[i] / agg.cellN[i]).toFixed(3)) : null);
        }
        out.cells.push(cells);
        // 跨天日期分隔线: 在日期变更的第一个桶处标记
        if (prevDay !== null && day !== prevDay) {
            out.dayMarkers.push({ x: bucketIdx / (totalBuckets - 1), day: day });
        }
        prevDay = day;
    }

    // 离线区段: 相邻桶间隔 > 桶长 → 断传, 用上一桶有效电压画灰色虚线。
    // 现在已生成完整连续桶, 此处主要兼容旧逻辑; 无数据桶已在 volt 中填 null。
    for (let i = 0; i < out.bucketTs.length; i++) {
        if (i === 0) { out.off.push(null); continue; }
        const dt = out.bucketTs[i] - out.bucketTs[i - 1];
        if (dt > bucket && out.volt[i - 1] > 0) out.off.push(out.volt[i - 1]);
        else out.off.push(null);
    }
    return out;
}

/* ===== 2026-08-09 跨天统计日期分隔线插件 =====
 * 横轴跨自然日时, 在日期变更点绘制竖直分隔标线 + 顶部标注 MM-DD;
 * 标线颜色随双主题(data-theme)自适应(日光浅灰/夜视浅灰蓝);
 * 用法: 曲线渲染后调用 _applyDayMarkers(chart, dayMarkers) 写入跨天点,
 *       chart.dayMarkers = [{x: 像素比例0~1, day: "MM-DD"}] */
const DaySeparatorPlugin = {
    id: "daySeparator",
    afterDraw(chart) {
        const markers = chart.dayMarkers;
        if (!markers || !markers.length || !chart.chartArea) return;
        const { left, right, top, bottom } = chart.chartArea;
        const ctx = chart.ctx;
        // 双主题适配: 夜视(默认)=浅灰蓝线+亮白字; 日光=浅灰线+深黑字
        const isLight = (document.documentElement.getAttribute("data-theme") || "dark") === "light";
        const lineColor = isLight ? "rgba(120,130,140,0.7)" : "rgba(150,180,210,0.8)";
        const textColor = isLight ? "#1a202c" : "#e2e8f0";
        ctx.save();
        ctx.lineWidth = 1;
        markers.forEach(m => {
            const x = left + m.x * (right - left);
            ctx.strokeStyle = lineColor;
            ctx.setLineDash([4, 3]);
            ctx.beginPath();
            ctx.moveTo(x, top);
            ctx.lineTo(x, bottom);
            ctx.stroke();
            ctx.setLineDash([]);
            // 标线上方标注 MM-DD(层级置顶, 不被波形遮挡)
            ctx.fillStyle = textColor;
            ctx.font = "bold 10px sans-serif";
            ctx.textAlign = "center";
            ctx.fillText(m.day, x, top + 12);
        });
        ctx.restore();
    },
};

/* 辅助: 将跨天点写入 chart(供 DaySeparatorPlugin 绘制)
 * dayMarkers: [{x: 0~1 像素比例, day: "MM-DD"}] */
function _applyDayMarkers(chart, dayMarkers) {
    if (chart) chart.dayMarkers = dayMarkers || [];
}

/* ===== 2026-08-11: 坐标轴自适应辅助(根治"坐标消失") =====
 * 有数据时 zoom(设 min/max 放大到数据范围); 无数据时 fallback(给一个可见的
 * 坐标框架 suggestedMin/Max, 不清空坐标轴), 避免曲线全空时 Y 轴塌成 0~1 看不见. */
function _axisZoom(axis, lo, hi) {
    if (!axis) return;
    axis.suggestedMin = undefined;
    axis.suggestedMax = undefined;
    axis.min = lo;
    axis.max = hi;
}
function _axisFallback(axis, lo, hi) {
    if (!axis) return;
    axis.min = undefined;
    axis.max = undefined;
    axis.suggestedMin = lo;
    axis.suggestedMax = hi;
}

/* ===== 2026-08-09 悬停十字线交互插件 =====
 * 鼠标悬停曲线时绘制竖直参考线 + 顶部显示该位置时间, 配合 Chart.js tooltip
 * 显示所有通道数值(解决"鼠标移动看不到所在位置数据"); 双主题适配线色 */
const CrosshairPlugin = {
    id: "crosshair",
    afterInit(chart) {
        chart._crossPos = null;
    },
    afterEvent(chart, evt) {
        const pos = evt && evt.x;
        if (evt && evt.type === "pointermove") {
            chart._crossPos = pos;
        } else if (evt && evt.type === "pointerout") {
            chart._crossPos = null;
        }
    },
    afterDraw(chart) {
        if (chart._crossPos == null || !chart.chartArea) return;
        const { left, right, top, bottom } = chart.chartArea;
        const x = chart._crossPos;
        if (x < left || x > right) return;
        const ctx = chart.ctx;
        const isLight = (document.documentElement.getAttribute("data-theme") || "dark") === "light";
        ctx.save();
        ctx.strokeStyle = isLight ? "rgba(90,100,110,0.6)" : "rgba(220,230,240,0.55)";
        ctx.lineWidth = 1;
        ctx.setLineDash([3, 3]);
        ctx.beginPath();
        ctx.moveTo(x, top);
        ctx.lineTo(x, bottom);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.restore();
        // 2026-08-10 修复: 删除 chart.draw()——afterDraw 内再次整图重绘会覆盖/干扰
        //   Chart.js tooltip 的绘制(悬停时看不到数据), 十字线在当前绘制帧内已画出,
        //   无需二次重绘; 且 draw() 递归调用还会造成额外的性能开销.
    },
};

/* 2026-08-09: 注册十字线插件(全局生效) */
if (typeof Chart !== "undefined" && Chart.register) {
    try {
        Chart.register(DaySeparatorPlugin, CrosshairPlugin);
    } catch (e) { /* 插件注册失败不影响核心渲染 */ }
}

/* 辅助: 从 labels 自动收集跨天点(含 "MM-DD HH:mm" 前缀的标签即跨天变更点)
 * 供 DaySeparatorPlugin 在日期变更处绘制竖直分隔标线; 不侵入曲线内部循环
 * 2026-08-09 防重叠: 跨天点密集(如 7d 每天一个)时均匀稀疏化到 MAX_MARKERS 个,
 * 避免 MM-DD 文字堆叠遮挡波形; 稀疏区间(< 上限)完整展示全部日期标注 */
function _collectDayMarkersFromLabels(labels) {
    const markers = [];
    if (!Array.isArray(labels) || labels.length < 2) return markers;
    const n = labels.length - 1;
    const re = /^(\d{2}-\d{2}) /;   // 匹配 "MM-DD HH:mm" 前缀
    labels.forEach((lb, i) => {
        const m = re.exec(String(lb));
        if (m && i > 0) {
            markers.push({ x: i / n, day: m[1] });
        }
    });
    // 防重叠稀疏化: 超过 MAX_MARKERS 个跨天点时均匀抽样(保留首尾)
    const MAX_MARKERS = 7;
    if (markers.length > MAX_MARKERS) {
        const step = (markers.length - 1) / (MAX_MARKERS - 1);
        const sampled = [];
        for (let k = 0; k < MAX_MARKERS; k++) {
            sampled.push(markers[Math.round(k * step)]);
        }
        return sampled;
    }
    return markers;
}

/* 2026-08-09: 注册跨天日期分隔线插件(全局生效, 所有曲线共用) */
if (typeof Chart !== "undefined" && Chart.register) {
    try { Chart.register(DaySeparatorPlugin); } catch (e) { /* 插件注册失败不影响核心渲染 */ }
}

/* 2026-08-11 优化: 通知 Toast 容器(右下角堆叠, 不遮挡顶栏按钮/内容)
 *   - 用独立 #toastWrap 容器承载多条 toast, 避免单条覆盖;
 *   - toast 自身 pointer-events:none, 永不拦截点击(仅展示);
 *   - 容器固定在右下角, 不与左上导航/右上按钮(刷新/诊断/通知)重叠. */
function showToast(msg, type = "info") {
    let wrap = $("toastWrap");
    if (!wrap) {
        wrap = document.createElement("div");
        wrap.id = "toastWrap";
        wrap.style.cssText = "position:fixed;right:16px;bottom:16px;z-index:9999;display:flex;flex-direction:column;gap:8px;align-items:flex-end;max-width:min(92vw,360px);pointer-events:none;";
        document.body.appendChild(wrap);
    }
    const t = document.createElement("div");
    t.style.cssText = "pointer-events:auto;padding:10px 14px;border-radius:8px;background:#152030;border:1px solid #1e3048;color:#e2e8f0;font-size:13px;line-height:1.4;box-shadow:0 10px 40px rgba(0,0,0,0.5);opacity:0;transform:translateY(8px);transition:opacity .25s,transform .25s;max-width:100%;word-break:break-word;";
    t.textContent = msg;
    const bg = { info: "#1a2840", success: "rgba(16,185,129,0.15)", error: "rgba(239,68,68,0.15)", warn: "rgba(245,158,11,0.15)" }[type] || "#1a2840";
    const border = { info: "#3b82f6", success: "#10b981", error: "#ef4444", warn: "#f59e0b" }[type] || "#1e3048";
    t.style.background = bg;
    t.style.borderColor = border;
    wrap.appendChild(t);
    requestAnimationFrame(() => { t.style.opacity = "1"; t.style.transform = "translateY(0)"; });
    setTimeout(() => {
        t.style.opacity = "0";
        t.style.transform = "translateY(8px)";
        setTimeout(() => t.remove(), 300);
    }, 3000);
}

/* ====== 2026-08-09: 浏览器通知(Notification API, 对标 PWA 桌面通知) ======
 * 故障/离线时即使页面不在前台也能弹系统通知, 需用户授权(首次调用请求)
 * 开关持久化 localStorage: bms_notify_enabled (1=开 0=关, 默认开) */
function notifyBrowser(title, body, icon = "⚠️") {
    try {
        if (localStorage.getItem("bms_notify_enabled") === "0") return;
        if (!("Notification" in window)) return;
        if (Notification.permission === "default") {
            Notification.requestPermission();      // 首次请求授权(不阻塞)
            return;
        }
        if (Notification.permission !== "granted") return;
        const n = new Notification(icon + " " + title, {
            body: body,
            tag: "bms-alert-" + Date.now(),
            requireInteraction: false,
        });
        n.onclick = () => { window.focus(); n.close(); };
        setTimeout(() => n.close(), 10000);        // 10s 自动关闭
    } catch (e) { /* 通知失败不阻塞页面 */ }
}

/* 浏览器通知开关切换(前端控制面板按钮调用) */
function toggleBrowserNotify() {
    const on = localStorage.getItem("bms_notify_enabled") !== "0";
    if (on) {
        localStorage.setItem("bms_notify_enabled", "0");
        showToast("浏览器通知已关闭", "info");
    } else {
        localStorage.setItem("bms_notify_enabled", "1");
        if ("Notification" in window && Notification.permission === "default") {
            Notification.requestPermission();
        }
        showToast("浏览器通知已开启", "success");
    }
    const btn = $("notifyToggleBtn");
    if (btn) {
        btn.textContent = localStorage.getItem("bms_notify_enabled") === "0" ? "🔕 浏览器通知:关" : "🔔 浏览器通知:开";
    }
}

/* ====== 2026-08-22 改进(C1): 告警声音 —— 离线/故障时短促蜂鸣提醒, 无需音频文件 ======
 * 开关与浏览器通知共用 localStorage: bms_notify_enabled (0=关 1=开, 默认开);
 * 页面首次交互前浏览器可能阻止 AudioContext, 用一次性恢复(resume)兼容. */
let _alertCtx = null;
function playAlertSound(beeps = 2) {
    try {
        if (localStorage.getItem("bms_notify_enabled") === "0") return;
        if (!window.AudioContext && !window.webkitAudioContext) return;
        if (!_alertCtx) _alertCtx = new (window.AudioContext || window.webkitAudioContext)();
        if (_alertCtx.state === "suspended") _alertCtx.resume();
        const ctx = _alertCtx;
        const t0 = ctx.currentTime;
        for (let i = 0; i < beeps; i++) {
            const osc = ctx.createOscillator();
            const gain = ctx.createGain();
            osc.type = "sine";
            osc.frequency.value = 880;                 // A5 蜂鸣
            gain.gain.setValueAtTime(0.001, t0 + i * 0.3);
            gain.gain.exponentialRampToValueAtTime(0.25, t0 + i * 0.3 + 0.02);
            gain.gain.exponentialRampToValueAtTime(0.001, t0 + i * 0.3 + 0.18);
            osc.connect(gain).connect(ctx.destination);
            osc.start(t0 + i * 0.3);
            osc.stop(t0 + i * 0.3 + 0.2);
        }
    } catch (e) { /* 声音失败不阻塞页面 */ }
}

/* 离线/故障告警: 声音 + 桌面通知(带节流, 同一状态每 60s 最多提醒一次, 防刷屏) */
let _alertState = { online: null, fault: null, lastTs: 0 };
function alertOnStateChange(online, fault) {
    try {
        if (localStorage.getItem("bms_notify_enabled") === "0") return;
        const now = Date.now();
        // 状态变化(在线→离线 / 故障 0→非0)才触发; 60s 节流防反复刷
        const onlineChanged = (online !== _alertState.online);
        const faultChanged = (fault !== _alertState.fault);
        _alertState.online = online;
        _alertState.fault = fault;
        if (!onlineChanged && !faultChanged) return;
        const isBad = (online === false) || (fault && fault !== 0);
        if (!isBad) return;                            // 恢复在线/故障清除不打扰
        if (now - _alertState.lastTs < 60000) return;  // 60s 节流
        _alertState.lastTs = now;
        if (online === false) {
            playAlertSound(3);
            notifyBrowser("设备离线", "BMS 设备已离线(" + new Date().toLocaleTimeString() + "), 请检查", "📡");
        } else if (fault) {
            playAlertSound(2);
            notifyBrowser("设备故障", "检测到故障码 0x" + fault.toString(16), "⚠️");
        }
    } catch (e) { /* 不阻塞 */ }
}

// 命令日志(右下角 command-log)
function addLog(msg, cls = "log-info") {
    const log = $("cmdLog");
    if (!log) return;
    const time = fmtNow();
    const div = document.createElement("div");
    div.innerHTML = '<span class="' + cls + '">[' + time + '] ' + htmlEsc(msg) + '</span>';
    log.appendChild(div);
    log.scrollTop = log.scrollHeight;
    while (log.children.length > MAX_LOG_LINES) log.removeChild(log.firstChild);
}

// 运行日志(logContainer) —— 本地显示 + 后端持久化(防清空/篡改)
function addOperationLog(msg, level = "info") {
    logCounter++;
    const nowMs = Date.now();
    const entry = { id: logCounter, msg, level, time: fmtNow(), recvTime: nowMs / 1000 };
    operationLog.push(entry);
    if (operationLog.length > 500) operationLog.shift();
    _opLogQueue.push({ recv_time: entry.recvTime, level: level, msg: msg });
    if (_opLogQueue.length >= 50) flushOperationLog(true);
    else _scheduleOpLogFlush();
    renderOperationLog();
}

function _scheduleOpLogFlush() {
    if (_opLogFlushTimer) return;
    _opLogFlushTimer = setTimeout(() => {
        _opLogFlushTimer = null;
        flushOperationLog(true);
    }, OP_LOG_FLUSH_MS);
}

function flushOperationLog(force = false) {
    if (_opLogQueue.length === 0) return;
    if (_opLogFlushTimer && force) { clearTimeout(_opLogFlushTimer); _opLogFlushTimer = null; }
    const batch = _opLogQueue.splice(0, _opLogQueue.length);
    fetch("/api/operation_log", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ entries: batch }),
        credentials: "same-origin"
    }).then(r => { if (!r.ok) console.warn("operation_log flush failed", r.status); })
      .catch(err => console.warn("operation_log flush error", err));
}

function renderOperationLog() {
    const container = $("logContainer");
    if (!container) return;
    const filter = $("logLevel") ? $("logLevel").value : "all";
    const filtered = filter === "all" ? operationLog : operationLog.filter(l => l.level === filter);
    let html = "";
    for (let i = filtered.length - 1; i >= 0; i--) {
        const l = filtered[i];
        const color = { info: "var(--accent-blue)", warn: "var(--accent-yellow)", error: "var(--accent-red)", success: "var(--accent-green)" }[l.level] || "var(--text-secondary)";
        const persistBadge = l.persistent ? '<span title="已持久化到服务端" style="margin-left:6px;font-size:10px;color:var(--text-muted)">☁</span>' : "";
        html += '<div style="display:flex;gap:10px;padding:6px 0;border-bottom:1px solid var(--border-color);font-size:12px">' +
                '<span style="color:var(--text-muted);min-width:70px">' + l.time + '</span>' +
                '<span style="color:' + color + '">' + (LOG_ICON[l.level] || "🔵") + " " + htmlEsc(l.msg) + persistBadge + '</span>' +
                '</div>';
    }
    container.innerHTML = html || '<div style="color:var(--text-muted);padding:20px;text-align:center">暂无记录</div>';
    container.scrollTop = container.scrollHeight;
}

function clearLogsDisplay() {
    operationLog.length = 0;
    logCounter = 0;
    renderOperationLog();
    addOperationLog("运行日志显示已清空(数据库记录不受影响)", "info");
}

function _initOpLogRangeDefaults() {
    if (_opLogRangeInited) return;
    const sEl = $("opLogStart"), eEl = $("opLogEnd");
    if (!sEl || !eEl) return;
    const toLocal = d => {
        const off = d.getTimezoneOffset() * 60000;
        return new Date(d.getTime() - off).toISOString().slice(0, 16);
    };
    const now = new Date();
    eEl.value = toLocal(now);
    sEl.value = toLocal(new Date(now.getTime() - 7 * 86400 * 1000));
    _opLogRangeInited = true;
}

function loadOperationLog(addBootMsg) {
    _initOpLogRangeDefaults();
    const end = Math.floor(Date.now() / 1000);
    const start = end - 7 * 86400;
    const url = "/api/operation_log?start=" + start + "&end=" + end + "&limit=500";
    fetch(url, { cache: "no-store" })
        .then(r => r.json())
        .then(res => {
            if (!res || !res.ok) return;
            const rows = res.data || [];
            operationLog = rows.map((r, idx) => ({
                id: -(idx + 1),
                msg: r.msg,
                level: r.level,
                time: fmtDateTime(r.recv_time),
                recvTime: r.recv_time,
                persistent: true
            }));
            logCounter = operationLog.length;
            if (addBootMsg) addOperationLog("系统启动,等待设备数据...", "info");
            renderOperationLog();
        })
        .catch(err => console.warn("loadOperationLog failed", err));
}

function exportOperationLogServer() {
    const sEl = $("opLogStart"), eEl = $("opLogEnd");
    let start = null, end = null;
    if (sEl && sEl.value) {
        const t = new Date(sEl.value).getTime();
        if (!isNaN(t)) start = Math.floor(t / 1000);
    }
    if (eEl && eEl.value) {
        const t = new Date(eEl.value).getTime();
        if (!isNaN(t)) end = Math.floor(t / 1000);
    }
    if (end === null) end = Math.floor(Date.now() / 1000);
    if (start === null) start = end - 7 * 86400;
    if (start > end) { showToast("起始时间不能晚于结束时间", "warn"); return; }
    const level = ($("logLevel") && $("logLevel").value !== "all") ? $("logLevel").value : null;
    let url = "/api/operation_log/export?start=" + start + "&end=" + end;
    if (level) url += "&level=" + level;
    showToast("正在从服务器导出运行日志...", "info");
    fetch(url, { cache: "no-store" })
        .then(r => {
            if (!r.ok) return r.json().then(j => { throw new Error((j && j.msg) || ("HTTP " + r.status)); });
            return r.blob();
        })
        .then(blob => {
            const a = document.createElement("a");
            a.href = URL.createObjectURL(blob);
            a.download = "BMS_运行日志_" + new Date(start * 1000).toISOString().slice(0, 10) + "_" + new Date(end * 1000).toISOString().slice(0, 10) + ".csv";
            document.body.appendChild(a); a.click(); a.remove();
            setTimeout(() => URL.revokeObjectURL(a.href), 1000);
            addOperationLog("导出运行日志 CSV(服务器)", "success");
            showToast("运行日志已导出(服务器 · 持久化)", "success");
        })
        .catch(err => showToast("导出失败: " + err.message, "error"));
}

// 页面卸载前把未 flush 的运行日志批次强制 sendBeacon 到后端
function _setupOpLogBeforeUnload() {
    window.addEventListener("beforeunload", () => {
        if (_opLogQueue.length === 0) return;
        try {
            const blob = new Blob([JSON.stringify({ entries: _opLogQueue })], { type: "application/json" });
            navigator.sendBeacon("/api/operation_log", blob);
        } catch (e) { /* noop */ }
    });
}
_setupOpLogBeforeUnload();

// ============================================================
// Socket.IO 动态加载(参考 HTML 未引入,需自动注入)
// ============================================================
function loadSocketIO(callback) {
    if (window.io) { callback(); return; }
    const s = document.createElement("script");
    // 2026-08-08: 优先本地库(与 index.html 的 vendor 一致), 避免外网 CDN 加载失败导致无实时推送
    s.src = "/static/vendor/socket.io.min.js";
    s.onload = callback;
    s.onerror = () => {
        addLog("Socket.IO 加载失败,实时数据不可用", "log-error");
        addOperationLog("Socket.IO CDN 加载失败", "error");
    };
    document.head.appendChild(s);
}

// ============================================================
// 故障码解析(bitmask -> 告警数组)
// ============================================================
function parseFault(faultCode) {
    const code = Number(faultCode || 0);
    if (code === 0) return [];
    const alerts = [];
    for (const bitStr in FAULT_MAP) {
        const bit = Number(bitStr);
        if ((code & bit) === bit) {
            alerts.push({ code: bit, level: FAULT_MAP[bit].level, desc: FAULT_MAP[bit].desc });
        }
    }
    return alerts;
}

function levelToIcon(level) {
    return { ok: "🟢", warn: "🟡", error: "🔴", fatal: "⛔" }[level] || "🟢";
}

function levelToClass(level) {
    return { ok: "info", warn: "warning", error: "critical", fatal: "critical" }[level] || "info";
}

// ============================================================
// KPI 卡片: 历史缓存徽章/灰度 (问题4修复)
// ============================================================
function _applyKpiCacheBadge(isCached, ageMs) {
    /* 对顶部 5 个 KPI 卡片加徽章/灰度, no_data/实时 时清除.
       离线数据 (DB缓存) => 半透明数值 + 左上角橙色 "历史缓存·Nmin前" 徽章 */
    const kpiCards = document.querySelectorAll(".kpi-card");
    if (!kpiCards || !kpiCards.length) return;
    kpiCards.forEach(card => {
        /* 移除旧徽章 */
        const old = card.querySelector(".cache-badge");
        if (old) { old.remove(); }
        /* 数值主色调 */
        const mainVal = card.querySelector(".kpi-value");
        if (mainVal) {
            // 2026-08-18 界面改进: 缓存时数值仅轻微降对比度(原 0.68 太糊),
            //   保留可读性, 由右上角徽章承担提示职责
            if (isCached) mainVal.style.opacity = "0.88";
            else mainVal.style.opacity = "";
        }
    });
    if (!isCached) return;
    /* 组装 age 文本 */
    let ageText = "";
    if (!ageMs || ageMs <= 0) ageText = "历史数据";
    else if (ageMs < 60000) ageText = Math.round(ageMs/1000) + "s前";
    else if (ageMs < 3600000) ageText = (ageMs/60000).toFixed(0) + "min前";
    else ageText = (ageMs/3600000).toFixed(1) + "h前";
    kpiCards.forEach(card => {
        const badge = document.createElement("span");
        badge.className = "cache-badge";
        badge.textContent = "缓存·" + ageText;
        Object.assign(badge.style, {
            position: "absolute", top: "8px", right: "10px",
            fontSize: "10px", padding: "2px 7px",
            borderRadius: "999px",
            /* 2026-08-18 界面改进: 去掉刺眼的橙色高亮框, 改为低调深色小标签,
             *   与暗色主题融为一体, 避免与告警黄/橙色混淆(那才是真正要警示的) */
            background: "rgba(255,255,255,0.06)",
            color: "var(--text-muted, #9ca3af)",
            border: "1px solid rgba(255,255,255,0.1)",
            letterSpacing: "0",
            pointerEvents: "none",
        });
        const cs = getComputedStyle(card).position;
        if (cs !== "relative" && cs !== "absolute") {
            card.style.position = "relative";
        }
        card.appendChild(badge);
    });
}

// ============================================================
// KPI 卡片更新
// ============================================================
// 2026-08-10 修复(F6): CRC 校验失败顶部横幅。被篡改/位翻转的数据仍会照常显示,
//   仅用小字提示不够, 此处额外弹出红色醒目横幅, 直到数据恢复完整才消失。
function showCrcBanner(crcOk) {
    const id = "crcAlertBanner";
    let el = document.getElementById(id);
    if (crcOk === false) {
        if (!el) {
            el = document.createElement("div");
            el.id = id;
            // 2026-08-11 优化: 横幅下移到顶栏(62px)之下, 且 pointer-events:none 不拦截点击,
            //   不再遮挡顶部导航/刷新/诊断/通知等按钮与页面内容.
            el.style.cssText =
                "position:fixed;top:62px;left:0;right:0;z-index:9998;" +
                "background:#e53935;color:#fff;font-weight:700;text-align:center;" +
                "padding:8px 12px;font-size:14px;box-shadow:0 2px 8px rgba(0,0,0,.3);" +
                "pointer-events:none;";
            document.body.appendChild(el);
        }
        el.textContent = "⚠ 数据完整性校验失败(CRC 不匹配): 当前显示数据可能存在位翻转或被篡改, 请勿据此操作!";
    } else if (el) {
        el.remove();
    }
}

// 2026-08-11: 总电压/总电流 vs 5分钟前 变化量用的内存环形缓冲(实时数据每 1~2s 上报)
let _kpiBuf = [];

function updateKPI(p) {
    // ===== 2026-08-10 D6: 数据完整性(CRC)校验状态展示 =====
    //   后端已按固件同算法复算比对(fault/mos/balanceOn/workState/chargeMode/timestamp),
    //   前端仅展示结果: true=数据完整 false=校验失败(篡改/位翻转) null/undefined=旧固件未上报
    const crcEl = $("crcStatus");
    if (crcEl) {
        if (p.crc_ok === false) {
            crcEl.textContent = "CRC异常";
            crcEl.style.color = "var(--accent-red)";
            crcEl.title = "上报数据完整性校验失败(CRC 不匹配), 可能存在位翻转/篡改!";
        } else if (p.crc_ok === true) {
            crcEl.textContent = "CRC完整";
            crcEl.style.color = "var(--accent-green)";
            crcEl.title = "上报数据完整性校验通过(CRC8+CRC32)";
        } else {
            crcEl.textContent = "CRC旧固件";
            crcEl.style.color = "var(--text-muted)";
            crcEl.title = "固件未上报 crc 字段(需烧录含 CRC 的新固件)";
        }
    }
    // 2026-08-10 修复(F6): CRC 失败时顶部醒目横幅告警(原仅 KPI 小字, 易被忽略)
    showCrcBanner(p.crc_ok);
    // ===== 2026-08-10 D3: 控制命令回比(设备状态确认) =====
    try { _checkCmdVerify(p); } catch (e) { console.warn("cmd verify fail:", e); }
    // ===== no_data: 设备离线/传感器未接, KPI 显示"--" =====
    if (p.no_data) {
        if ($("voltInt")) $("voltInt").textContent = "--";
        if ($("voltDec")) $("voltDec").textContent = "";
        if ($("currInt")) $("currInt").textContent = "--";
        if ($("currDec")) $("currDec").textContent = "";
        if ($("socVal")) $("socVal").textContent = "--";
        if ($("socBar")) $("socBar").style.width = "0%";
        if ($("sohVal")) $("sohVal").textContent = "--";
        if ($("sohBar")) $("sohBar").style.width = "0%";
        if ($("pwrVal")) $("pwrVal").textContent = "--";
        if ($("pwrBar")) $("pwrBar").style.width = "0%";
        updateSohRing(0);
        // 状态文字
        const socStatus = $("socStatus");
        if (socStatus) socStatus.textContent = "无数据";
        const sohStatus = $("sohStatus");
        if (sohStatus) sohStatus.textContent = "无数据";
        const pwrStatus = $("pwrStatus");
        if (pwrStatus) pwrStatus.textContent = "无数据";
        const cycleEl = $("cycleCount");
        if (cycleEl) cycleEl.textContent = "--";
        // SOH 圆环和统计卡片
        const sohRingEl = $("sohRingVal");
        if (sohRingEl) sohRingEl.textContent = "--";
        const avgTempEl = $("avgTemp");
        if (avgTempEl) avgTempEl.textContent = "--";
        const estTimeEl = $("estTime");
        if (estTimeEl) estTimeEl.textContent = "--";
        const sohCapEl = $("sohCapacity");
        if (sohCapEl) sohCapEl.textContent = "--";
        const cellDiffEl = $("cellVoltDiff");
        if (cellDiffEl) cellDiffEl.textContent = "--";
        const tempDeltaEl = $("tempDelta");
        if (tempDeltaEl) tempDeltaEl.textContent = "--";
        const gradeEl = $("sohHealthGrade");
        if (gradeEl) { gradeEl.textContent = "--"; gradeEl.style.color = ""; gradeEl.style.background = ""; gradeEl.style.borderColor = ""; }
        // 离线: 告警徽标清零, 电压/电流变化量复位
        const _badgeOff = $("navAlarmBadge"); if (_badgeOff) { _badgeOff.textContent = "0"; _badgeOff.style.display = "none"; }
        if ($("voltDelta")) { $("voltDelta").textContent = "--"; $("voltDelta").parentElement.className = "kpi-change up"; }
        if ($("currDelta")) { $("currDelta").textContent = "--"; $("currDelta").parentElement.className = "kpi-change down"; }
        return;
    }
    // ===== Bug1/3 修复: cells 推导 pack_v/v_max/v_min 兜底 =====
    //   当 ESP32 上报 pack_v=0 但 cells (c1~c6) 有数据时,
    //   从单体重新累加 pack_v, 取 v_max/v_min (与 database.py query_latest 兜底一致)
    const cellsMv = [];
    if (p.cells && Array.isArray(p.cells) && p.cells.length) {
        p.cells.forEach(v => cellsMv.push(Number(v || 0)));
    } else {
        for (let i = 1; i <= 32; i++) {
            const v = Number(p["c" + i] || 0);
            if (v > 0) cellsMv.push(v);
        }
    }
    let packMv = Number(p.pack_v || 0);
    if (packMv <= 0 && cellsMv.some(v => v > 0)) {
        packMv = cellsMv.reduce((a, b) => a + b, 0);
    }
    let vMaxMv = Number(p.v_max || 0);
    if (vMaxMv <= 0 && cellsMv.some(v => v > 0)) {
        vMaxMv = Math.max(...cellsMv);
    }
    let vMinMv = Number(p.v_min || 0);
    if (vMinMv <= 0 && cellsMv.some(v => v > 0)) {
        vMinMv = Math.min(...cellsMv.filter(v => v > 0));
    }
    // ===== KPI 渲染 =====
    // 总电压 (mV -> V)
    const packV = packMv / 1000;
    const voltInt = Math.floor(packV);
    const voltDec = Math.round((packV - voltInt) * 10);
    if ($("voltInt")) $("voltInt").textContent = voltInt;
    if ($("voltDec")) $("voltDec").textContent = voltDec;
    // 总电流 (mA -> A, 正充电/负放电)
    // 2026-08-09 修复: 原 `Math.round((currAbs-currInt)*10)` 有进位 bug——
    //   2961mA→2.961A→Math.round(9.61)=10, 显示"2.10"而非进位 3.0, 且仅 1 位小数精度不足.
    //   改用 toFixed(2) 保留 2 位小数并正确进位(2961→"2.96"), 与华为云/推送一致.
    const current = Number(p.current || 0) / 1000;
    const currAbs = Math.abs(current);
    const _currFixed = currAbs.toFixed(2);            // "2.96"
    const _currParts = _currFixed.split(".");
    const currInt = _currParts[0] || "0";
    const currDec = _currParts[1] || "00";
    if ($("currInt")) $("currInt").textContent = currInt;
    if ($("currDec")) $("currDec").textContent = currDec;
    // SOC
    const soc = Number(p.soc || 0);
    if ($("socVal")) $("socVal").textContent = Math.round(soc);
    if ($("socBar")) $("socBar").style.width = Math.min(100, Math.max(0, soc)) + "%";
    // 2026-08-11: 侧边栏页脚 SOC 实时同步(顶部 KPI 已有 socVal/socBar, 此处补侧边栏)
    if ($("sfSoc")) $("sfSoc").textContent = Math.round(soc) + "%";
    if ($("sfBar")) $("sfBar").style.width = Math.min(100, Math.max(0, soc)) + "%";
    // SOH: 设备按百分比(0~100)上报(与 soc 一致, sys_mqtt_report 已 *100),
    //      前端直接用百分比显示, 不再做"<=1 强制 95"的兜底(那会把真实低SOH掩盖成95%,
    //      也曾在旧固件0~1上报时把 85% 误显为 95%). 离线/无数据由 no_data 路径显示"--".
    let soh = Number(p.soh || 0);
    // SOH 动态写入卡片(之前写死"95", 现在根据主控上报数据更新)
    if ($("sohVal")) $("sohVal").textContent = Math.round(soh);
    if ($("sohBar")) $("sohBar").style.width = Math.min(100, Math.max(0, soh)) + "%";
    // 瞬时功率 = V * A -> W (packV 是 V, current 是 A, 直接相乘)
    const power = Math.abs(packV * current);
    if ($("pwrVal")) $("pwrVal").textContent = Math.round(power);
    if ($("pwrBar")) $("pwrBar").style.width = Math.min(100, power / 1500 * 100).toFixed(0) + "%";
    // ===== 2026-08-11: 总电压/总电流 vs 5分钟前 变化量(用户反馈"数据没同步") =====
    //   实时数据每 1~2s 上报, 用内存环形缓冲取 ~5min 前样本做差, 无需新后端接口
    {
        const _now = Date.now();
        _kpiBuf.push({ t: _now, v: packV, a: current });
        while (_kpiBuf.length && _now - _kpiBuf[0].t > 600000) _kpiBuf.shift();
        let _ref = null;
        for (let i = _kpiBuf.length - 1; i >= 0; i--) {
            if (_now - _kpiBuf[i].t >= 300000) { _ref = _kpiBuf[i]; break; }
        }
        const _vd = $("voltDelta"), _cd = $("currDelta");
        if (_ref) {
            const _dv = packV - _ref.v, _da = current - _ref.a;
            if (_vd) { _vd.textContent = (_dv >= 0 ? "▲ " : "▼ ") + Math.abs(_dv).toFixed(2); _vd.parentElement.className = "kpi-change " + (_dv >= 0 ? "up" : "down"); }
            if (_cd) { _cd.textContent = (_da >= 0 ? "▲ " : "▼ ") + Math.abs(_da).toFixed(2); _cd.parentElement.className = "kpi-change " + (_da >= 0 ? "up" : "down"); }
        } else {
            if (_vd) { _vd.textContent = "--"; _vd.parentElement.className = "kpi-change up"; }
            if (_cd) { _cd.textContent = "--"; _cd.parentElement.className = "kpi-change down"; }
        }
    }
    // 侧边栏告警徽标: 活跃故障数(0 时隐藏)
    {
        const _badge = $("navAlarmBadge");
        if (_badge) {
            const _n = parseFault(p.fault).length;
            _badge.textContent = _n;
            _badge.style.display = _n > 0 ? "" : "none";
        }
    }
    // ===== 2026-08-09: 绝缘电阻 KPI 卡片(不平衡电桥法) =====
    //   数据源: 后端 /api/status 的 insulation_ohm_per_v(Ω/V)/insulation_rp/rn(Ω)
    //   判据: GB/T 18384.1 直流 >=100Ω/V 合格, <50Ω/V 保护(自动切断主回路)
    //   2026-08-13 优化: 卡片按状态加 alarm/warn 边框 + 闪烁报警徽标 + 阈值说明行
    {
        const insOv = Number(p.insulation_ohm_per_v || 0);
        const insRp = Number(p.insulation_rp || 0);
        const insRn = Number(p.insulation_rn || 0);
        const insVal = $("insulationVal");
        const insDet = $("insulationDetail");
        const insCard = $("insulationCard");
        const insBadge = $("insulationBadge");
        // 状态机: alarm(<50 保护) / warn(<100 预警) / ok(合格或有原始电阻) / unknown(未启用)
        let state = "unknown";
        if (insOv > 0) state = insOv < 50 ? "alarm" : (insOv < 100 ? "warn" : "ok");
        else if (insRp > 0 || insRn > 0) state = "ok";

        if (insVal) {
            if (insOv > 0) {
                insVal.innerHTML = insOv + '<span class="kpi-unit"> Ω/V</span>';
                insVal.style.color = state === "alarm" ? "var(--accent-red)"
                                  : state === "warn" ? "var(--accent-yellow)"
                                  : "var(--accent-green)";
            } else if (insRp > 0 || insRn > 0) {
                const rmin = (insRp > 0 && insRn > 0) ? Math.min(insRp, insRn)
                           : (insRp > 0 ? insRp : insRn);
                insVal.innerHTML = (rmin / 1000).toFixed(1) + '<span class="kpi-unit"> kΩ</span>';
                insVal.style.color = "var(--accent-cyan)";
            } else {
                insVal.innerHTML = "—<span class=\"kpi-unit\"> Ω/V</span>";
                insVal.style.color = "";
            }
        }
        if (insDet) {
            if (state === "alarm") {
                insDet.textContent = "⚠️ 绝缘保护 (<50Ω/V, 已切断主回路)";
                insDet.className = "kpi-change down";
            } else if (state === "warn") {
                insDet.textContent = "⚠️ 绝缘预警 (<100Ω/V)";
                insDet.className = "kpi-change down";
            } else if (insOv > 0) {
                insDet.textContent = "🟢 绝缘正常 (≥100Ω/V)";
                insDet.className = "kpi-change up";
            } else if (insRp > 0 || insRn > 0) {
                insDet.textContent = "Rp=" + (insRp/1000).toFixed(1) + "kΩ Rn=" + (insRn/1000).toFixed(1) + "kΩ";
                insDet.className = "kpi-change up";
            } else {
                insDet.textContent = "未启用/未接线";
                insDet.className = "kpi-change";
            }
        }
        // 卡片状态样式 + 报警徽标
        if (insCard) {
            insCard.classList.remove("kpi-card--alarm", "kpi-card--warn");
            if (state === "alarm") insCard.classList.add("kpi-card--alarm");
            else if (state === "warn") insCard.classList.add("kpi-card--warn");
        }
        if (insBadge) {
            if (state === "alarm") {
                insBadge.textContent = "⚠ 绝缘报警";
                insBadge.className = "kpi-badge";
                insBadge.style.display = "";
            } else if (state === "warn") {
                insBadge.textContent = "⚠ 绝缘预警";
                insBadge.className = "kpi-badge warn";
                insBadge.style.display = "";
            } else {
                insBadge.style.display = "none";
            }
        }
    }
    // ===== 2026-08-13: RS485/Modbus 通信状态 KPI 卡片 =====
    //   数据源: 后端 /api/status 的 rs485_online(1/0) / rs485_last_poll(秒,65535=从未) / rs485_err(累计错误)
    //   场景: BMS 为 Modbus RTU 从站, 外部主站(EMS/上位机)轮询读取; 卡片仅展示通信链路健康度
    {
        const rsOn   = Number(p.rs485_online ?? -1);   // -1=字段缺失(旧固件/未上报)
        const rsPoll = Number(p.rs485_last_poll ?? 65535);
        const rsErr  = Number(p.rs485_err ?? 0);
        const rsVal  = $("rs485Val");
        const rsDet  = $("rs485Detail");
        const rsCard = $("rs485Card");
        const rsBadge= $("rs485Badge");

        if (rsVal) {
            if (rsOn === -1) {
                rsVal.innerHTML = "—";
                rsVal.style.color = "";
            } else if (rsOn === 1) {
                // 2026-08-18 界面改进: 去 emoji 红/绿点, 与 MQTT/TLS 等通信卡片纯文字风格一致
                rsVal.textContent = "在线";
                rsVal.style.color = "var(--accent-green)";
            } else {
                rsVal.textContent = "离线";
                rsVal.style.color = "var(--accent-red)";
            }
        }
        if (rsDet) {
            if (rsOn === -1) {
                rsDet.textContent = "固件未上报";
                rsDet.className = "kpi-change";
            } else if (rsPoll >= 65535) {
                rsDet.textContent = "从未被主站轮询 · 累计错误 " + rsErr;
                rsDet.className = "kpi-change";
            } else {
                rsDet.textContent = "最近轮询 " + rsPoll + "s 前 · 累计错误 " + rsErr;
                rsDet.className = rsOn === 1 ? "kpi-change up" : "kpi-change down";
            }
        }
        if (rsCard) {
            // 2026-08-18 界面改进: 去掉离线时的黄色警示边框(kpi-card--warn),
            //   避免与其他卡片风格割裂; 离线状态由文字颜色+detail 表达即可
            rsCard.classList.remove("kpi-card--warn");
        }
        if (rsBadge) {
            // 2026-08-18 界面改进: 离线徽章(kpi-badge 红底闪烁)与整体风格割裂且与
            //   rsVal 红色"离线"文字重复, 一律隐藏, 状态交给值文字+detail 表达
            rsBadge.style.display = "none";
        }
    }
    // 电压条(6S 满充 25.2V, 但 UI 卡片原用 60V 满量程. 保留 60V, 对 6S 就是 ~42%)
    if ($("voltBar")) $("voltBar").style.width = Math.min(100, packV / 60 * 100).toFixed(0) + "%";
    // 电流条(满量程 25A)
    if ($("currBar")) $("currBar").style.width = Math.min(100, currAbs / 25 * 100).toFixed(0) + "%";
    // ============================================================
    // QoL 修复: KPI 状态文字 **不再单靠电流猜**, 采用「硬件回读 MOS + 电流方向 + 故障码」联合判定
    //   新固件(charge_mos != -1) 优先看真实开关回读, 保证下发停止充电命令后立即变"停止充电(待执行)"
    //   老固件(charge_mos = -1) 退回原电流阈值判定 (保持兼容性)
    // ============================================================
    const chgMos = Number(p.charge_mos ?? -1);      // -1=未知 0=断开 1=吸合
    const dsgMos = Number(p.discharge_mos ?? -1);   // -1=未知 0=断开 1=吸合
    const balOn  = Number(p.balance_on  ?? (Number(p.balance_mask ?? 0) !== 0 ? 1 : 0));
    const chgMode= Number(p.charge_mode ?? 0);
    const faultMask = Number(p.fault || 0);
    const hasChargeProt  = !!(faultMask & (0x00002 | 0x00020));   // 单体过压保护/充电过流保护
    const hasDischargeProt = !!(faultMask & (0x00008 | 0x00080)); // 单体欠压保护/放电过流保护
    const hasThermalFault = !!(faultMask & 0x00400);              // 热失控
    const currentA = current;                                      // mA->A 上面已算

    function _kpiStatusSet(text, cls /* up/down/warn/idle */) {
        const el = $("socStatus"); if (!el) return;
        el.textContent = text;
        el.className = "kpi-change " + (cls || "");
    }
    // ---- SOC 卡状态 ----
    if (hasThermalFault) {
        _kpiStatusSet("⚠️ 热失控切断", "error");
    } else if (chgMos === 0 && dsgMos === 0) {
        // 两个MOS都断: 明确用户下发了停止或保护动作
        if (hasChargeProt)      _kpiStatusSet("充电保护中", "warn");
        else if (hasDischargeProt) _kpiStatusSet("放电保护中", "warn");
        else                    _kpiStatusSet("待机(充放MOS关)", "idle");
    } else if (balOn === 1 && Math.abs(currentA) < 0.1) {
        // 均衡单独进行
        _kpiStatusSet("均衡中", "warn");
    } else if (chgMos === 1) {
        // 充电MOS吸合: 看电流(固件约定: 正=放电 负=充电)
        if (currentA < -0.1)        _kpiStatusSet("充电中", "up");
        else if (currentA > 0.1)   _kpiStatusSet("异常(充电MOS却放电)", "error");
        else                        _kpiStatusSet("充电MOS开(待充电)", "up");
    } else if (dsgMos === 1) {
        if (currentA > 0.1)        _kpiStatusSet("放电中", "down");
        else if (currentA < -0.1)  _kpiStatusSet("异常(放电MOS却充电)", "error");
        else                        _kpiStatusSet("放电MOS开(待负载)", "down");
    } else {
        // 老固件(-1) 或 未知: 退回原电流阈值判定(固件约定: 正=放电 负=充电)
        if (currentA > 0.1)        _kpiStatusSet("放电中", "down");
        else if (currentA < -0.1)  _kpiStatusSet("充电中", "up");
        else                        _kpiStatusSet("静置", "");
    }

    // ---- 功率卡状态 (第5张卡) ----
    const pwrChangeEl = $("pwrStatus") || document.querySelectorAll(".kpi-card .kpi-change")[4];
    if (pwrChangeEl) {
        let txt = "静置", cls = "";
        // 2026-08-09 修复: 与固件约定一致(正=放电 负=充电)
        if (chgMos === 1 || currentA < -0.1)      { txt = "充电";   cls = "up"; }
        else if (dsgMos === 1 || currentA > 0.1)  { txt = "放电";   cls = "down"; }
        else if (balOn === 1)                     { txt = "均衡";   cls = "warn"; }
        if (hasThermalFault)                      { txt = "热失控"; cls = "error"; }
        pwrChangeEl.textContent = txt;
        pwrChangeEl.className = "kpi-change " + cls;
    }

    // ---- SOH 卡状态 (原写死 "良好" → 动态判: 故障码+SOH百分比) ----
    const sohChangeEl = $("sohStatus") || document.querySelectorAll(".kpi-card .kpi-change")[3];
    if (sohChangeEl) {
        const sohWarn = !!(faultMask & 0x40000);     // FAULT_SOH_DECAY = 0x40000 (健康度衰减预警)
        let txt = "良好", cls = "up";
        if (hasThermalFault)          { txt = "热失控";   cls = "error"; }
        else if (soh < 60)             { txt = "需更换";   cls = "error"; }
        else if (soh < 80)             { txt = "衰减严重"; cls = "warn"; }
        else if (soh < 95 || sohWarn)  { txt = "轻度衰减"; cls = "warn"; }
        else if (soh < 99)             { txt = "良好";     cls = "up"; }
        else                           { txt = "极佳";     cls = "up"; }
        sohChangeEl.textContent = txt;
        sohChangeEl.className = "kpi-change " + cls;
    }
    // SOH 圆环(SVG)
    updateSohRing(soh);
    // SOH 卡片数值
    const sohValEl = $("sohRingVal");
    if (sohValEl) sohValEl.textContent = soh.toFixed(0) + "%";
    // SOH KPI 卡片: sohVal 已在上方通过 $("sohVal").textContent 更新, 此处只更新进度条
    // 注意: 不能用 innerHTML 替换 .kpi-value, 否则会破坏 <span id="sohVal"> 导致 no_data 时无法设为 "--"
    const sohKpi = document.querySelectorAll(".kpi-row .kpi-card")[3];
    if (sohKpi) {
        const sohBarKpi = sohKpi.querySelector(".kpi-bar-fill");
        if (sohBarKpi) sohBarKpi.style.width = soh + "%";
    }
    // 平均温度(用于 SOH stats). temp_max/min 单位 0.1℃ -> ℃
    const tMax = Number(p.temp_max || 0), tMin = Number(p.temp_min || 0);
    const tempAvg = (tMax + tMin) / 2 / 10;
    const tempStatEl = $("avgTemp");
    if (tempStatEl) tempStatEl.textContent = tempAvg > 0 ? tempAvg.toFixed(1) + "°" : "--";
    // 循环次数: 来自主控 (安时积分法计算)
    if ($("cycleCount")) {
        const cyc = Number(p.cycle_count || 0);
        $("cycleCount").textContent = cyc;
    }
    // 充满预计时间(按 6S 2.5Ah 真实容量估算, 原假设 100Ah 偏差过大)
    if ($("estTime")) {
        const NOMINAL_AH = 2.5;   /* 与 PARAM_META.capacity_ah / bms_config.h 一致 */
        if (current > 0.1 && soc < 100) {
            const remainAh = Math.max(0, (100 - soc) / 100 * NOMINAL_AH);
            const hours = remainAh / Math.abs(current);
            const h = Math.floor(hours);
            const m = Math.floor((hours - h) * 60);
            $("estTime").textContent = h + "h" + m + "m";
        } else if (current < -0.1 && soc > 0) {
            const remainAh = Math.max(0, soc / 100 * NOMINAL_AH);
            const hours = remainAh / Math.abs(current);
            const h = Math.floor(hours);
            const m = Math.floor((hours - h) * 60);
            $("estTime").textContent = h + "h" + m + "m";
        } else {
            $("estTime").textContent = "--";
        }
    }
    // 健康度卡片新增指标: 容量估算 / 单体最大压差 / 温度极差 / 健康评级
    const NOMINAL_AH = 2.5;
    if ($("sohCapacity")) {
        $("sohCapacity").textContent = (NOMINAL_AH * soh / 100).toFixed(2) + " Ah";
    }
    if ($("cellVoltDiff")) {
        const vMax = Number(p.v_max || 0), vMin = Number(p.v_min || 0);
        const diff = (vMax > 0 && vMin > 0) ? (vMax - vMin) : 0;
        $("cellVoltDiff").textContent = diff > 0 ? diff + " mV" : "--";
    }
    if ($("tempDelta")) {
        const tMax = Number(p.temp_max || 0), tMin = Number(p.temp_min || 0);
        const delta = (tMax !== 0 || tMin !== 0) ? Math.abs(tMax - tMin) / 10 : 0;
        $("tempDelta").textContent = delta > 0 ? delta.toFixed(1) + " °C" : "--";
    }
    if ($("sohHealthGrade")) {
        const gradeInfo = _sohGrade(soh);
        const el = $("sohHealthGrade");
        el.textContent = gradeInfo.label;
        el.style.color = gradeInfo.color;
        el.style.background = gradeInfo.bg;
        el.style.borderColor = gradeInfo.border;
    }
}

// SOH 健康评级工具
function _sohGrade(soh) {
    if (soh >= 95) return { label: "A 优秀", color: "var(--accent-green)", bg: "rgba(16,185,129,0.12)", border: "rgba(16,185,129,0.25)" };
    if (soh >= 85) return { label: "B 良好", color: "var(--accent-cyan)", bg: "rgba(6,182,212,0.12)", border: "rgba(6,182,212,0.25)" };
    if (soh >= 70) return { label: "C 一般", color: "var(--accent-yellow)", bg: "rgba(245,158,11,0.12)", border: "rgba(245,158,11,0.25)" };
    return { label: "D 需更换", color: "var(--accent-red)", bg: "rgba(239,68,68,0.12)", border: "rgba(239,68,68,0.25)" };
}

// SOH 圆环(SVG stroke-dashoffset)
function updateSohRing(soh) {
    const ring = document.querySelector(".soh-ring circle:nth-child(2)");
    if (!ring) return;
    const circumference = 2 * Math.PI * 68; // r=68
    const offset = circumference * (1 - soh / 100);
    ring.setAttribute("stroke-dasharray", circumference);
    ring.setAttribute("stroke-dashoffset", offset);
    // 颜色根据 SOH 等级
    let color = COLOR.purple;
    if (soh < 70) color = COLOR.red;
    else if (soh < 90) color = COLOR.yellow;
    ring.setAttribute("stroke", color);
    const valEl = document.querySelector(".soh-ring-val");
    if (valEl) valEl.style.color = color;
}

// ============================================================
// ============= 曲线图表三层封装(2026-08-09 重构) =============
// 第1层 工具类层 ChartUtil: 纯函数(数据判定/换算/提示文案), 无副作用
// 第2层 图表组件层 ChartComponent: 图表渲染/占位提示封装
// 第3层 页面业务层: initCellVoltChart / loadCellVoltHistory / pushCellVoltChart
// ============================================================
const ChartUtil = {
    /* 全 0 判定: cells 全为 0 或空 → true(BQ76952 未接/未启用) */
    isAllZeroCells(cells) {
        return Array.isArray(cells) && cells.length > 0 && cells.every(v => Number(v || 0) === 0);
    },
    /* 单元格换算: mV → V(无效值返回 null) */
    mvToV(mv) {
        const n = Number(mv || 0);
        return n > 0 ? Number((n / 1000).toFixed(3)) : null;
    },
    /* 占位提示文案(按场景返回) */
    hintText(kind) {
        const map = {
            "no_data": "单体电压全为 0(检查 BQ76952 接线 / HW_ENABLE_BQ76952 配置)",
            "timeout": "历史数据请求超时(15s), 请检查后端/网络后重试",
            "format":  "历史数据格式异常, 无法渲染",
            "empty":   "该时间段无单体电压历史数据(设备未上报/未接线)",
        };
        return map[kind] || "暂无数据";
    },
};

const ChartComponent = {
    /* 占位提示组件: 统一控制 cellVoltEmptyHint 显隐与文案(页面业务层调用) */
    showHint(msg, show) {
        const el = $("cellVoltEmptyHint");
        if (!el) return;
        el.textContent = msg;
        el.style.display = show ? "block" : "none";
    },
};

/* ===== 2026-08-09 双主题(日光高亮/夜视低蓝光) =====
 * 切换 body[data-theme=light|dark], 覆盖 :root CSS 变量实现主题;
 * localStorage 记忆选择, 刷新后保持; 夜视默认(车载低蓝光) */
function toggleTheme() {
    const root = document.documentElement;
    const cur = root.getAttribute("data-theme") || "dark";
    const next = cur === "dark" ? "light" : "dark";
    root.setAttribute("data-theme", next);
    try { localStorage.setItem("bms_theme", next); } catch (e) {}
    const btn = $("themeToggleBtn");
    if (btn) btn.textContent = next === "light" ? "🌙 夜视" : "🌞 日光";
    // 2026-08-09: 主题切换后重绘全部曲线, 让日期分隔线/文字对比度立即适配新主题
    // 2026-09-15 可读性修复: 重绘前先刷新网格/刻度配色(静态配置仅建图时求值,
    //   不刷新则切主题后旧图表仍是旧配色). 网格统一取主题色; 刻度仅替换灰色
    //   文字(COLOR.textMut/#94a3b8), 多轴强调色(cyan/green/yellow)保留色码语义.
    try {
        const _g = themeGridColor(), _m = themeMutedColor();
        [mainChart, cellVoltChart, historyChart, trendChart, sohTrendChart].forEach(ch => {
            if (ch && ch.options && ch.options.scales) {
                Object.values(ch.options.scales).forEach(sc => {
                    if (sc && sc.grid && sc.grid.color) sc.grid.color = _g;
                    if (sc && sc.ticks && sc.ticks.color &&
                        (sc.ticks.color === COLOR.textMut || sc.ticks.color === "#94a3b8")) {
                        sc.ticks.color = _m;
                    }
                });
            }
            if (ch && typeof ch.draw === "function") ch.draw();
        });
    } catch (e) { /* 重绘异常忽略 */ }
    addLog("[主题] 已切换为" + (next === "light" ? "日光高亮" : "夜视低蓝光") + "模式", "log-info");
}

// ============================================================
// 单体数据表格
// ============================================================
/* ====== 2026-08-12: 真 3D 电池组视图 (Three.js WebGL 引擎) ======
 * 替代旧版手写 Canvas 圆柱引擎: 真实透视相机 + 光照 + 软阴影;
 * 电芯按"状态"着色(绿=正常 黄=预警 红=异常), 均衡中电芯附加青色自发光;
 * 支持 拖拽旋转 / 滚轮缩放 / 悬停查看逐芯 电压·温度·均衡·状态;
 * 正面/侧面/俯视/等距 四视角预设按钮(setBattery3DView)。库本地化于 static/vendor/three.min.js。 */

function bms3dStatusOf(i, arr, ovV, uvV, dvMv) {
    const mv = arr[i] || 0; const vV = mv / 1000;
    const valid = arr.filter(v => v > 0).map(v => v / 1000);
    const avgV = valid.length ? valid.reduce((a, b) => a + b, 0) / valid.length : 0;
    if (vV > 0 && avgV > 0) {
        const d = Math.abs(Math.round((vV - avgV) * 1000));
        if (vV > ovV) return "error";
        if (vV < uvV) return "warn";
        if (d >= dvMv) return "warn";
        return "normal";
    }
    if (vV > ovV) return "error";
    if (vV < uvV) return "warn";
    return "normal";
}

const B3D = {
    inited: false, failed: false,
    scene: null, camera: null, renderer: null, group: null, _shell: null,
    cellMeshes: [], cellData: [],
    raycaster: null, pointer: null,
    n: 0, lastCells: [],
    packView: "iso", focusIdx: -1,
    drag: null, autoRotate: true,
    curRot: { x: 0, y: 0 }, targetRot: { x: 0, y: 0 },
    camTarget: null, zoom: 1,
    layout: { totalW: 200, totalD: 200 },

    statusColor(s) { return s === "error" ? 0xff3b3b : s === "warn" ? 0xf5a623 : 0x27bf63; },
    sizeW() { const cv = document.getElementById("b3dCanvas"); const par = cv ? cv.parentElement : null; return par ? par.clientWidth || 600 : 600; },
    sizeH() { return 340; },

    init() {
        if (this.inited) return true;
        if (typeof THREE === "undefined") { this.failed = true; this._showErr("Three.js 未加载(刷新重试)"); return false; }
        const cv = document.getElementById("b3dCanvas");
        if (!cv) { this.failed = true; return false; }
        const W = this.sizeW(), H = this.sizeH();
        if (W < 4) return false; // 容器不可见, 延迟到 ResizeObserver 再初始化
        try {
            this.renderer = new THREE.WebGLRenderer({ canvas: cv, antialias: true, alpha: true });
            this.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
            this.renderer.setSize(W, H, false);
            this.renderer.shadowMap.enabled = true;
            this.renderer.shadowMap.type = THREE.PCFSoftShadowMap;
            if (THREE.sRGBEncoding !== undefined) this.renderer.outputEncoding = THREE.sRGBEncoding;

            this.scene = new THREE.Scene();
            this.camera = new THREE.PerspectiveCamera(45, W / H, 0.1, 6000);
            this.camTarget = new THREE.Vector3(0, 60, this._dist());
            this.camera.position.copy(this.camTarget);
            this.camera.lookAt(0, 0, 0);

            this.scene.add(new THREE.AmbientLight(0xffffff, 0.55));
            const dir = new THREE.DirectionalLight(0xffffff, 0.95);
            dir.position.set(-140, 240, 180);
            dir.castShadow = true;
            dir.shadow.mapSize.set(1024, 1024);
            dir.shadow.bias = -0.0006;
            const sc = dir.shadow.camera; sc.left = -240; sc.right = 240; sc.top = 240; sc.bottom = -240; sc.near = 1; sc.far = 1200; sc.updateProjectionMatrix();
            this.scene.add(dir);
            const fill = new THREE.DirectionalLight(0x88aaff, 0.28);
            fill.position.set(160, -30, -140);
            this.scene.add(fill);

            const ground = new THREE.Mesh(new THREE.PlaneGeometry(900, 900), new THREE.ShadowMaterial({ opacity: 0.22 }));
            ground.rotation.x = -Math.PI / 2; ground.position.y = -64; ground.receiveShadow = true;
            this.scene.add(ground);

            this.group = new THREE.Group();
            this.scene.add(this.group);

            // 电池组外壳(半透明亚克力框)
            this._shell = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
                new THREE.MeshStandardMaterial({ color: 0x1b2740, transparent: true, opacity: 0.08, metalness: 0.2, roughness: 0.6 }));
            this._shell.visible = false;
            this.group.add(this._shell);

            this.raycaster = new THREE.Raycaster();
            this.pointer = new THREE.Vector2();

            this._bindEvents(cv);
            this.inited = true;
            this.setView(this.packView);
            this._loop();
            return true;
        } catch (e) {
            console.error("[B3D] init error", e);
            this.failed = true; this._showErr("3D 初始化失败(WebGL 不可用): " + ((e && e.message) || e));
            return false;
        }
    },

    _dist() { const L = this.layout || {}; return Math.max(L.totalW || 200, L.totalD || 200) * 1.45 + 280; },

    buildCells(n) {
        if (!this.group) return;
        for (let i = this.group.children.length - 1; i >= 0; i--) {
            const c = this.group.children[i];
            if (c !== this._shell) this.group.remove(c);
        }
        this.cellMeshes = [];
        if (!n) return;
        const perRow = n <= 6 ? n : Math.ceil(Math.sqrt(n * 1.7));
        const rows = Math.ceil(n / perRow);
        const R = 11, Hh = 46, gx = 30, gy = 30;
        const geo = new THREE.CylinderGeometry(R, R, Hh, 30, 1);
        const capGeo = new THREE.CylinderGeometry(R * 0.42, R * 0.42, 4, 20);
        const totalW = (perRow - 1) * gx, totalD = (rows - 1) * gy;
        this.layout = { perRow, rows, R, Hh, gx, gy, totalW, totalD };
        for (let i = 0; i < n; i++) {
            const r = Math.floor(i / perRow), c = i % perRow;
            const x = -totalW / 2 + c * gx, z = -totalD / 2 + r * gy;
            const mat = new THREE.MeshStandardMaterial({ color: 0x27bf63, metalness: 0.35, roughness: 0.38, emissive: 0x000000, emissiveIntensity: 0 });
            const m = new THREE.Mesh(geo, mat);
            m.castShadow = true; m.receiveShadow = true;
            m.position.set(x, 0, z);
            m.userData.cellIndex = i;
            this.group.add(m);
            this.cellMeshes.push(m);
            const cap = new THREE.Mesh(capGeo, new THREE.MeshStandardMaterial({ color: 0xb8c0cc, metalness: 0.9, roughness: 0.25 }));
            cap.position.set(x, Hh / 2 + 2, z);
            cap.userData.cellIndex = i;
            this.group.add(cap);
            const spr = this._labelSprite("C" + (i + 1));
            spr.position.set(x, -Hh / 2 - 10, z);
            this.group.add(spr);
            m.userData.label = spr;
        }
        const pad = R + 14;
        this._shell.geometry.dispose();
        this._shell.geometry = new THREE.BoxGeometry(totalW + pad * 2, Hh + pad * 2, totalD + pad * 2);
        this._shell.visible = true;
        // 阵列尺寸变化后, 按新布局重算相机距离(保留当前观察方向, 不重置用户旋转)
        if (this.camera && this.camTarget) {
            const dir = this.camera.position.clone();
            if (dir.lengthSq() < 1e-6) dir.set(0, 0.3, 1);
            dir.normalize();
            this.camTarget = dir.multiplyScalar(this._dist() * this.zoom);
        }
    },

    _labelSprite(text) {
        const cv = document.createElement("canvas"); cv.width = 128; cv.height = 64;
        const ctx = cv.getContext("2d");
        // 圆角药丸底色, 让序号在任何电芯颜色上都清晰可读(色块角标)
        const w = 96, h = 40, x = (128 - w) / 2, y = (64 - h) / 2, r = 12;
        ctx.fillStyle = "rgba(10,15,28,0.82)";
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.arcTo(x + w, y, x + w, y + h, r);
        ctx.arcTo(x + w, y + h, x, y + h, r);
        ctx.arcTo(x, y + h, x, y, r);
        ctx.arcTo(x, y, x + w, y, r);
        ctx.closePath();
        ctx.fill();
        ctx.strokeStyle = "rgba(159,179,200,0.55)"; ctx.lineWidth = 2; ctx.stroke();
        ctx.font = "bold 30px Consolas, monospace"; ctx.textAlign = "center"; ctx.textBaseline = "middle";
        ctx.fillStyle = "#cfe0f0"; ctx.fillText(text, 64, 34);
        const tex = new THREE.CanvasTexture(cv);
        const sp = new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, transparent: true, depthWrite: false, depthTest: false }));
        sp.scale.set(34, 17, 1);
        sp.renderOrder = 999;
        return sp;
    },

    updateColors() {
        for (let i = 0; i < this.cellMeshes.length; i++) {
            const d = this.cellData[i] || {};
            const mat = this.cellMeshes[i].material;
            mat.color.setHex(this.statusColor(d.status));
            if (d.bal) { mat.emissive.setHex(0x00e5ff); mat.emissiveIntensity = 0.55; }
            else if (d.status === "error") { mat.emissive.setHex(0xff3b3b); mat.emissiveIntensity = 0.5; }
            else if (d.status === "warn") { mat.emissive.setHex(0xf5a623); mat.emissiveIntensity = 0.3; }
            else { mat.emissive.setHex(0x000000); mat.emissiveIntensity = 0; }
        }
    },

    setView(view) {
        this.packView = view || "iso";
        const dist = this._dist();
        let pos;
        if (view === "front") pos = new THREE.Vector3(0, 40, dist);
        else if (view === "side") pos = new THREE.Vector3(dist, 40, 0);
        else if (view === "top") pos = new THREE.Vector3(0, dist, 0.01);
        else pos = new THREE.Vector3(dist * 0.78, dist * 0.62, dist * 0.78);
        this.camTarget = pos.clone().multiplyScalar(this.zoom);
        this.targetRot = { x: 0, y: 0 };
    },

    _bindEvents(cv) {
        const wrap = cv.parentElement;
        cv.addEventListener("mousedown", e => {
            this.drag = { x: e.clientX, y: e.clientY, ry: this.targetRot.y, rx: this.targetRot.x };
            this.autoRotate = false;
            this._syncAutoBtn();
        });
        window.addEventListener("mouseup", () => { this.drag = null; });
        cv.addEventListener("mousemove", e => {
            const rect = cv.getBoundingClientRect();
            const x = e.clientX - rect.left, y = e.clientY - rect.top;
            if (this.drag) {
                this.targetRot.y = this.drag.ry + (e.clientX - this.drag.x) * 0.01;
                this.targetRot.x = Math.max(-1.3, Math.min(1.3, this.drag.rx - (e.clientY - this.drag.y) * 0.01));
                this._hideTip();
                return;
            }
            this._hover(x, y, rect, wrap);
        });
        cv.addEventListener("mouseleave", () => this._hideTip());
        cv.addEventListener("click", e => {
            const rect = cv.getBoundingClientRect();
            const i = this._pick(e.clientX - rect.left, e.clientY - rect.top, rect);
            if (i >= 0) { this.focusIdx = (this.focusIdx === i) ? -1 : i; this._highlight(); }
        });
        cv.addEventListener("wheel", e => {
            e.preventDefault();
            const k = e.deltaY > 0 ? 1.08 : 0.92;
            this.zoom = Math.max(0.55, Math.min(2.2, this.zoom * k));
            if (this.camTarget) this.camTarget.multiplyScalar(k);
        }, { passive: false });
    },

    _ndc(x, y, rect) { this.pointer.x = (x / rect.width) * 2 - 1; this.pointer.y = -(y / rect.height) * 2 + 1; },
    _pick(x, y, rect) {
        if (!this.raycaster || !this.cellMeshes.length) return -1;
        this._ndc(x, y, rect);
        this.raycaster.setFromCamera(this.pointer, this.camera);
        const hits = this.raycaster.intersectObjects(this.cellMeshes, false);
        return hits.length ? hits[0].object.userData.cellIndex : -1;
    },
    _hover(x, y, rect, wrap) { const i = this._pick(x, y, rect); if (i < 0) { this._hideTip(); return; } this._showTip(i, x, y, rect, wrap); },
    _showTip(i, x, y, rect, wrap) {
        const d = this.cellData[i]; if (!d) return;
        const tip = document.getElementById("b3dTip"); if (!tip) return;
        const stTxt = d.status === "error" ? "异常" : d.status === "warn" ? "预警" : "正常";
        const stCol = d.status === "error" ? "#ff3b3b" : d.status === "warn" ? "#f5a623" : "#27bf63";
        const balTxt = d.bal ? "是" : "否";
        const tempTxt = d.tempC != null ? d.tempC.toFixed(1) + "°C" : "--";
        tip.innerHTML = '<div class="tt-h">电芯 #' + (i + 1) + '</div>' +
            '<div class="tt-r"><span>电压</span><b>' + d.vV.toFixed(3) + ' V</b></div>' +
            '<div class="tt-r"><span>温度</span><b>' + tempTxt + '</b></div>' +
            '<div class="tt-r"><span>均衡</span><b style="color:' + (d.bal ? "#00e5ff" : "inherit") + '">' + balTxt + '</b></div>' +
            '<div class="tt-r"><span>状态</span><b style="color:' + stCol + '">' + stTxt + '</b></div>';
        tip.style.display = "block";
        let l = x + 14, t = y + 14;
        if (l + tip.offsetWidth > rect.width) l = x - tip.offsetWidth - 14;
        if (t + tip.offsetHeight > rect.height) t = y - tip.offsetHeight - 14;
        tip.style.left = Math.max(2, l) + "px"; tip.style.top = Math.max(2, t) + "px";
    },
    _hideTip() { const tip = document.getElementById("b3dTip"); if (tip) tip.style.display = "none"; },
    _highlight() { for (let i = 0; i < this.cellMeshes.length; i++) { const f = (i === this.focusIdx) ? 1.14 : 1.0; this.cellMeshes[i].scale.set(f, f, f); } },
    _syncAutoBtn() { const b = document.getElementById("b3dAutoBtn"); if (b) b.classList.toggle("active", this.autoRotate); },

    onResize() {
        if (!this.inited) { if (this.n) this.applyData(); return; }
        const W = this.sizeW(), H = this.sizeH();
        if (W < 4) return;
        this.renderer.setSize(W, H, false);
        this.camera.aspect = W / H; this.camera.updateProjectionMatrix();
    },

    applyData() {
        const W = this.sizeW();
        if (W < 4) return; // 容器不可见, 等 ResizeObserver
        if (!this.inited) { if (!this.init()) return; }
        if (this.cellMeshes.length !== this.n) this.buildCells(this.n);
        this.updateColors();
        this._highlight();
        this.renderPack();
    },

    renderPack() { if (this.inited && this.renderer) this.renderer.render(this.scene, this.camera); },

    _loop() {
        if (!this.inited) return;
        requestAnimationFrame(() => this._loop());
        this.curRot.y += (this.targetRot.y - this.curRot.y) * 0.15;
        this.curRot.x += (this.targetRot.x - this.curRot.x) * 0.15;
        this.group.rotation.y = this.curRot.y;
        this.group.rotation.x = this.curRot.x;
        if (this.autoRotate && !this.drag) this.targetRot.y += 0.0025;
        if (this.camTarget) this.camera.position.lerp(this.camTarget, 0.12);
        this.camera.lookAt(0, 0, 0);
        this.renderer.render(this.scene, this.camera);
    },

    _showErr(msg) { const el = document.getElementById("b3dErr"); if (el) { el.style.display = "block"; el.textContent = msg; } }
};

function ensureBattery3DDOM() {
    const el = document.getElementById("battery3D");
    if (!el || el.dataset.built) return;
    el.dataset.built = "1";
    el.innerHTML =
        '<div class="b3d-wrap">' +
            '<canvas id="b3dCanvas"></canvas>' +
            '<div id="b3dTip" class="b3d-tip"></div>' +
            '<div class="b3d-legend">' +
                '<span><i style="background:#27bf63"></i>正常</span>' +
                '<span><i style="background:#f5a623"></i>预警</span>' +
                '<span><i style="background:#ff3b3b"></i>异常</span>' +
                '<span><i style="background:#00e5ff"></i>均衡中</span>' +
                '<span class="b3d-hint">拖拽旋转 · 滚轮缩放 · 悬停/点击查看</span>' +
            '</div>' +
            '<div id="b3dSummary" class="b3d-summary"></div>' +
            '<div id="b3dErr" class="b3d-err" style="display:none"></div>' +
        '</div>';
    // DOM 被重建(如数据先空后非空)时, 强制 Three 用新 canvas 重新初始化
    if (B3D.inited && B3D.renderer && B3D.renderer.domElement !== document.getElementById("b3dCanvas")) {
        B3D.inited = false; B3D.failed = false; B3D.cellMeshes = []; B3D.renderer = null; B3D.scene = null;
    }
    const autoBtn = document.getElementById("b3dAutoBtn");
    if (autoBtn) autoBtn.classList.toggle("active", B3D.autoRotate);
    if (window.ResizeObserver) { new ResizeObserver(() => B3D.onResize()).observe(el); }
}

function _syncPackViewButtons(view) {
    const names = { front: "正面", side: "侧面", top: "俯视", iso: "等距" };
    const card = document.getElementById("battery3D");
    const btns = (card && card.closest(".chart-card")) ? card.closest(".chart-card").querySelectorAll(".chart-actions .command-btn") : document.querySelectorAll(".chart-actions .command-btn");
    btns.forEach(b => { if (b.id === "b3dAutoBtn") return; b.classList.toggle("active", b.textContent.trim() === names[view]); });
}

function setBattery3DView(view) {
    B3D.packView = view || "iso";
    B3D.setView(B3D.packView);
    _syncPackViewButtons(B3D.packView);
}

function toggleBattery3DAutoRotate() {
    B3D.autoRotate = !B3D.autoRotate;
    B3D._syncAutoBtn();
}

function renderBattery3D(cells) {
    ensureBattery3DDOM();
    const el = document.getElementById("battery3D");
    if (!el) return;
    // 2026-08-21 修复(#4 串数与 3D 不一致): 3D 渲染数量与串数配置(NUM_CELLS)统一。
    //   原实现用 cells.length(设备上报格数), 设备上报格数少于用户配置串数时
    //   (如配置 9 串、设备只报 6 格), 3D 数量与顶部串数/单体电压曲线不一致。
    //   现按 NUM_CELLS 渲染, 未上报格补 0, 与 renderCellTable/单体曲线行为一致。
    let arr = (cells || []).slice();
    const n = NUM_CELLS;
    if (!(n >= 1)) {
        el.innerHTML = '<div class="b3d-empty">等待电芯数据...</div>';
        delete el.dataset.built;
        return;
    }
    while (arr.length < n) arr.push(0);      // 不足补 0(与 renderCellTable 一致)
    arr = arr.slice(0, n);                   // 超出截断
    if (arr.every(v => !v || v === 0)) {
        // 全 0: 传感器未接或设备离线 —— 仍按配置串数渲染骨架(0V), 顶部状态点反映在线/离线
        //   原逻辑直接显示"等待电芯数据", 导致 3D 数量与串数对不上; 改为渲染骨架
    }
    // 阈值: 与后端 PARAM_META/bms_config.h 一致, 回退输入框, 最终回退主控默认值
    const PM = window._BMS_PARAMS || {};
    const getEl = id => document.getElementById(id);
    const ovV = PM.cell_ov_prot_mv ? (PM.cell_ov_prot_mv.value / 1000) : (parseFloat(getEl("thOverVolt") ? getEl("thOverVolt").value : 4.25) || 4.25);
    const uvV = PM.cell_uv_prot_mv ? (PM.cell_uv_prot_mv.value / 1000) : (parseFloat(getEl("thUnderVolt") ? getEl("thUnderVolt").value : 2.80) || 2.80);
    const dvMv = PM.cell_dv_warn_mv ? Number(PM.cell_dv_warn_mv.value) : (parseInt(getEl("thDvWarn") ? getEl("thDvWarn").value : 100, 10) || 100);
    const temps = (window.lastPayload && Array.isArray(window.lastPayload.temps)) ? window.lastPayload.temps : null;
    const mask = (window.lastPayload && Number(window.lastPayload.balance_mask || 0)) || 0;
    B3D.cellData = [];
    for (let i = 0; i < n; i++) {
        const mv = arr[i] || 0;
        const tempC = (temps && temps[i] != null) ? temps[i] / 10 : null;
        const bal = ((mask >> i) & 1) ? true : false;
        B3D.cellData.push({ mv, vV: mv / 1000, tempC, bal, status: bms3dStatusOf(i, arr, ovV, uvV, dvMv) });
    }
    B3D.n = n; B3D.lastCells = arr;
    const valid = arr.filter(v => v > 0);
    const maxV = valid.length ? Math.max.apply(null, valid) : 0;
    const minV = valid.length ? Math.min.apply(null, valid) : 0;
    const sum = document.getElementById("b3dSummary");
    if (sum) sum.innerHTML = '最高 <b style="color:var(--accent-green)">' + (maxV / 1000).toFixed(3) + 'V</b> · 最低 <b style="color:var(--accent-red)">' + (minV / 1000).toFixed(3) + 'V</b> · 压差 <b>' + ((maxV - minV) / 1000).toFixed(3) + 'V</b>';
    B3D.applyData();
    _syncPackViewButtons(B3D.packView);
}


function renderCellTable(cells) {
    const tbody = $("cellTableBody");
    if (!tbody) return;
    const arr = (cells || []).slice(0, NUM_CELLS);
    while (arr.length < NUM_CELLS) arr.push(0);

    // 全0判定: 传感器未接或设备离线, 表格显示"无数据"
    const allZero = arr.every(v => !v || v === 0);
    if (allZero) {
        let html = "";
        for (let i = 0; i < NUM_CELLS; i++) {
            html += '<tr data-cell="' + (i + 1) + '">' +
                '<td><strong>#' + (i + 1) + '</strong></td>' +
                '<td style="color:var(--text-muted)">--</td>' +
                '<td>--</td>' +
                '<td>--</td>' +
                '<td>--</td>' +
                '<td>--</td>' +
                '<td><span class="cell-indicator" style="background:var(--text-muted)">无数据</span></td>' +
                '<td><div class="cell-bar"><div class="cell-bar-track"><div class="cell-bar-fill" style="width:0%"></div></div></div></td>' +
                '</tr>';
        }
        tbody.innerHTML = html;
        return;
    }

    // 阈值优先从 _BMS_PARAMS 取(与后端 PARAM_META/bms_config.h 一致), 回退到输入框, 最终回退主控默认值
    const PM = window._BMS_PARAMS || {};
    const ov = PM.cell_ov_prot_mv ? (PM.cell_ov_prot_mv.value / 1000)
        : (parseFloat($("thOverVolt") ? $("thOverVolt").value : 4.25) || 4.25);
    const uv = PM.cell_uv_prot_mv ? (PM.cell_uv_prot_mv.value / 1000)
        : (parseFloat($("thUnderVolt") ? $("thUnderVolt").value : 2.80) || 2.80);
    const dv = PM.cell_dv_warn_mv ? Number(PM.cell_dv_warn_mv.value)
        : parseInt($("thDvWarn") ? $("thDvWarn").value : 100, 10) || 100;
    const avgTemp = lastPayload ? (Number(lastPayload.temp_max || 0) + Number(lastPayload.temp_min || 0)) / 2 / 10 : 30;
    // 每节温度(新物模型 temps 数组, 0.1℃); 无数组时温差列显示 "--" 而非恒 0
    const perCellTemps = (lastPayload && Array.isArray(lastPayload.temps)) ? lastPayload.temps : null;
    const vs = arr.filter(v => v > 0).map(v => v / 1000);
    const avgV = vs.length ? vs.reduce((a, b) => a + b, 0) / vs.length : 0;
    let html = "";

    for (let i = 0; i < NUM_CELLS; i++) {
        const vMv = arr[i];
        const vV = vMv / 1000;
        // 状态判定: 过压/欠压/压差过大 三级, 否则 normal
        let status = "normal";
        if (vMv > 0 && avgV > 0) {
            const diffMv = Math.abs(Math.round((vV - avgV) * 1000));
            if (vV > ov) status = "error";
            else if (vV < uv) status = "warn";
            else if (diffMv >= dv) status = "warn";
        } else {
            if (vV > ov) status = "error";
            else if (vV < uv) status = "warn";
        }
        // 该节实际温度(0.1℃ -> ℃), 无数据时温差列显示 "--"
        const cellTempRaw = (perCellTemps && perCellTemps[i] != null) ? Number(perCellTemps[i]) : NaN;
        const cellTempC = isNaN(cellTempRaw) ? null : cellTempRaw / 10;
        // 更新缓存
        cellData[i].voltage = vV;
        cellData[i].temp = cellTempC !== null ? cellTempC : avgTemp;
        cellData[i].soc = lastPayload ? Number(lastPayload.soc || 0) : 0;
        cellData[i].status = status;
        cellData[i].irq = 0;  // 真实 BMS 目前没采集内阻, 显示 0 (避免 mock 误导)
        cellData[i].deltaMv = vMv > 0 && avgV > 0 ? Math.round((vV - avgV) * 1000) : 0;
        const statusClass = status === "error" ? "error" : status === "warn" ? "warn" : "normal";
        const statusText = status === "error" ? "异常" : status === "warn" ? "预警" : "正常";
        const barColor = status === "error" ? "var(--accent-red)" : status === "warn" ? "var(--accent-yellow)" : "var(--accent-green)";
        const barWidth = Math.min(100, Math.max(0, (vV - 2.5) / 1.8 * 100)).toFixed(0);
        const tempDiff = cellTempC !== null ? Math.abs(cellTempC - avgTemp).toFixed(1) : "--";
        const irqText = cellData[i].irq ? (cellData[i].irq.toFixed(1) + "mΩ") : "--";
        const deltaText = cellData[i].deltaMv > 0 ? "+" + cellData[i].deltaMv + "mV"
            : (cellData[i].deltaMv < 0 ? cellData[i].deltaMv + "mV" : "±0mV");

        html += '<tr data-cell="' + (i + 1) + '" data-voltage="' + vV.toFixed(3) + '">' +
                '<td><strong>#' + (i + 1) + '</strong></td>' +
                '<td style="font-family:monospace;font-weight:600">' + vV.toFixed(3) + 'V</td>' +
                '<td style="font-family:monospace;font-size:11px">' + deltaText + '</td>' +
                '<td>' + irqText + '</td>' +
                '<td>' + tempDiff + '°C</td>' +
                '<td>' + cellData[i].soc.toFixed(1) + '%</td>' +
                '<td><span class="cell-indicator ' + statusClass + '">' + statusText + '</span></td>' +
                '<td><div class="cell-bar"><div class="cell-bar-track"><div class="cell-bar-fill" style="width:' + barWidth + '%;background:' + barColor + '"></div></div></div></td>' +
                '</tr>';
    }
    tbody.innerHTML = html;
}

function filterCells(q) {
    const rows = document.querySelectorAll("#cellTableBody tr");
    rows.forEach(r => {
        const num = r.dataset.cell || "";
        r.style.display = num.includes(q) ? "" : "none";
    });
}

// ============================================================
// 单体选择器(动态生成 6 个按钮)
// ============================================================
function buildCellSelector() {
    const wrap = $("cellSelector");
    if (!wrap) return;
    let html = '<button class="cell-select-btn active" onclick="setChartCell(-1)">全部</button>';
    for (let i = 0; i < NUM_CELLS; i++) {
        html += '<button class="cell-select-btn" onclick="setChartCell(' + i + ')">#' + (i + 1) + '</button>';
    }
    wrap.innerHTML = html;
}

let _cellSwitchCooldown = 0;  // 单体切换防抖时间戳

function setChartCell(idx) {
    selectedCellIndex = idx;
    document.querySelectorAll("#cellSelector .cell-select-btn").forEach((b, i) => {
        const btnIdx = i === 0 ? -1 : i - 1;
        b.classList.toggle("active", btnIdx === idx);
    });
    // 2026-08-08: 实时曲线标题提示当前显示对象(默认总电压/温度, 选中后显示单体号)
    const hintEl = $("mainChartHint");
    if (hintEl) hintEl.textContent = idx === -1 ? "总电压/温度" : "单体 #" + (idx + 1);
    if (idx === -1) {
        addLog("[视图] 显示全部单体电压/温度", "log-info");
    } else {
        addLog("[视图] 选择单体 #" + (idx + 1), "log-info");
    }
    // 立即刷新标签+重绘数据(不用等下一次MQTT/HTTP轮询)
    updateMainChartLabels();
    if (lastPayload && mainChart) {
        let voltV, tempC;
        const cells = lastPayload.cells || [];
        if (selectedCellIndex >= 0 && cells[selectedCellIndex] != null) {
            voltV = cells[selectedCellIndex] / 1000;
            tempC = (Number(lastPayload.temp_max || 0) + Number(lastPayload.temp_min || 0)) / 2 / 10;
        } else {
            voltV = Number(lastPayload.pack_v || 0) / 1000;
            tempC = Number(lastPayload.temp_max || 0) / 10;
        }
        // 立即填充所有数据点 + 重置曲线(加速切换体验)
        const curA = Number(lastPayload.current || 0) / 1000;
        for (let i = 0; i < MAX_CHART_POINTS; i++) {
            chartVoltData[i] = voltV;
            chartTempData[i] = tempC;
            chartCurrData[i] = curA;
            chartOfflineData[i] = null;   // 切换单体时重置离线指示线
        }
        _regenChartLabels(lastPayload.last_event_time_ms || Date.now());   // 重新生成标签(修复: 旧代码 fill("now") 看不到时间刻度)
        mainChart.data.labels = chartLabels.slice();
        mainChart.data.datasets[0].data = chartVoltData.slice();
        mainChart.data.datasets[1].data = chartTempData.slice();
        mainChart.data.datasets[2].data = chartCurrData.slice();
        mainChart.data.datasets[3].data = chartOfflineData.slice();
        // 动态调整Y轴(空数据保护: 避免全 null 时 Math.min 返回 Infinity)
        const validV = chartVoltData.filter(v => v !== null && !isNaN(v) && v > 0);
        const validT = chartTempData.filter(v => v !== null && !isNaN(v) && v > 0);
        if (validV.length > 0) {
            const vMin = Math.min(...validV);
            const vMax = Math.max(...validV);
            const vPad = Math.max(0.5, (vMax - vMin) * 0.3);
            mainChart.options.scales.y.min = Math.max(0, vMin - vPad);
            mainChart.options.scales.y.max = vMax + vPad;
        }
        if (validT.length > 0) {
            const tMin = Math.min(...validT);
            const tMax = Math.max(...validT);
            const tPad = Math.max(2, (tMax - tMin) * 0.3);
            mainChart.options.scales.y1.min = Math.max(0, tMin - tPad);
            mainChart.options.scales.y1.max = tMax + tPad;
        }
        mainChart.update("none");
    }
    // 主动触发云端数据刷新(加速获取最新数据, 防抖500ms)
    const now = Date.now();
    if (now - _cellSwitchCooldown > 500) {
        _cellSwitchCooldown = now;
        fetch("/api/refresh", { method: "POST" }).catch(() => {});
    }
}

function updateMainChartLabels() {
    if (!mainChart) return;
    if (selectedCellIndex < 0) {
        // Bug4 修复: 默认显示总电压(不是单体最高电压)
        mainChart.data.datasets[0].label = "总电压 (V)";
        mainChart.data.datasets[1].label = "最高温度 (°C)";
    } else {
        mainChart.data.datasets[0].label = "单体 #" + (selectedCellIndex + 1) + " 电压 (V)";
        mainChart.data.datasets[1].label = "单体 #" + (selectedCellIndex + 1) + " 温度 (°C)";
    }
    mainChart.update("none");
}

// Bug4 修复: 实时曲线时间范围切换(1分钟/5分钟/1小时)
function setMainChartRange(range, el) {
    mainChartRange = range;
    // 2026-08-11: 持久化当前范围, 刷新后由 initMainChartRange 恢复(含历史范围)
    try { localStorage.setItem("bms_main_range", range); } catch (e) {}
    // 切换 tab 高亮: 用 closest 找到 chart-actions 容器, 稳妥选择同组按钮
    if (el) {
        const container = el.closest(".chart-actions") || el.parentElement;
        container.querySelectorAll(".chart-tab").forEach(b => b.classList.remove("active"));
        el.classList.add("active");
    }
    const config = {
        "1min": { points: 30,   stepSec: 2,  unit: "s", maxTicks: 8,  label: "1 分钟" },
        "5min": { points: 150,  stepSec: 2,  unit: "s", maxTicks: 10, label: "5 分钟" },
    };
    // ===== 2026-08-10 修复: 主曲线历史查询模式(1h/6h/24h/7d) =====
    //   原 1h 是实时滚动窗口(从页面打开时刻起累积), 点击"1小时"看不到
    //   "当前时刻回退 1 小时"的历史数据; 现与 16 路曲线一致, 1h 也走历史查询
    const HISTORY_RANGES = { "1h": 60, "6h": 360, "24h": 1440, "7d": 10080 };
    if (HISTORY_RANGES[range]) {
        // 2026-08-09 修复: 历史模式标志——pushMainChart 每帧会覆盖 mainChart 数据,
        //   若不停用, 历史图刚渲染就被实时滚动数据冲掉("历史图没了").
        _mainChartHistMode = true;
        // 2026-08-11: 用户点击范围按钮 → 立即显示加载态(感知速度)
        _chartSetLoading(mainChart, true);
        // 2026-08-11 防抖 + 中断旧请求: 快速点击 1h/6h/24h/7d 时合并为最后一次请求,
        //   并 abort 前一个未完成的 fetch, 避免响应乱序/主线程被旧回调占用。
        if (_mainChartRangeTimer) clearTimeout(_mainChartRangeTimer);
        if (_mainChartHistoryAbort) { _mainChartHistoryAbort.abort(); _mainChartHistoryAbort = null; }
        _mainChartRangeTimer = setTimeout(() => {
            _mainChartRangeTimer = null;
            loadMainChartHistory(range, el, true);
        }, 250);
        return;
    }
    _mainChartHistMode = false;   // 切回实时范围: 恢复实时推送
    const cfg = config[range] || config["1min"];
    const maxPoints = cfg.points;
    // 调整数据缓冲长度(新补的点填 null, 标签重新生成对应时间偏移)
    while (chartVoltData.length < maxPoints) {
        chartVoltData.unshift(null);
        chartTempData.unshift(null);
        chartCurrData.unshift(null);
        chartOfflineData.unshift(null);
    }
    while (chartVoltData.length > maxPoints) {
        chartVoltData.shift();
        chartTempData.shift();
        chartCurrData.shift();
        chartOfflineData.shift();
    }
    // 重新生成 X 轴标签(最右=当前时刻, 向左依次按 stepSec 递减)
    _regenChartLabels(Date.now());
    // 数据与标签同步: 标签顺序是 [-298s,-296s,...,now] 共 maxPoints
    // chartVoltData 顺序也是 旧→新, pop 后 push 新值
    // 保证长度一致
    while (chartLabels.length > chartVoltData.length) chartLabels.shift();
    while (chartLabels.length < chartVoltData.length) chartLabels.unshift("");
    // 记录当前时间范围配置, 供 pushMainChart 重新生成标签使用
    _curRangeCfg = cfg;

    // 修复(2026-08-13): 切换实时范围(1min/5min)后**立即**用最近一帧 lastPayload 填满
    //   可见窗口, 保证切换即时"有反应". 此前仅依赖 seedMainChartRealtime 异步历史回填,
    //   而本设备电压自 8-06 停报(pack_v 恒0)、历史为空时回填全 null, 导致切 5min 后
    //   曲线长时间空白(用户反馈"反应不过来"); 下方 datasets[2] 也同步修正(曾误赋
    //   chartOfflineData 使电流线在切换后消失). seedMainChartRealtime 仍会在异步返回后
    //   用真实历史覆盖此 flat 线(更准). 逻辑与 setChartCell 完全一致.
    if (lastPayload) {
        const _cells = lastPayload.cells || [];
        let _v, _t, _c;
        if (selectedCellIndex >= 0 && _cells[selectedCellIndex] != null) {
            _v = _cells[selectedCellIndex] / 1000;
            _t = (Number(lastPayload.temp_max || 0) + Number(lastPayload.temp_min || 0)) / 2 / 10;
        } else {
            _v = Number(lastPayload.pack_v || 0) / 1000;
            _t = Number(lastPayload.temp_max || 0) / 10;
        }
        _c = Number(lastPayload.current || 0) / 1000;
        for (let i = 0; i < chartVoltData.length; i++) {
            chartVoltData[i] = _v > 0 ? _v : null;
            chartTempData[i] = _t;
            chartCurrData[i] = _c;
            chartOfflineData[i] = null;
        }
        // 动态调整 Y 轴(空数据保护, 避免全 null 时 Math.min 返回 Infinity)
        if (mainChart && mainChart.options && mainChart.options.scales) {
            const _vV = chartVoltData.filter(v => v !== null && !isNaN(v) && v > 0);
            const _vT = chartTempData.filter(v => v !== null && !isNaN(v) && v > 0);
            if (_vV.length > 0) {
                const vMin = Math.min.apply(null, _vV), vMax = Math.max.apply(null, _vV);
                const vPad = Math.max(0.5, (vMax - vMin) * 0.3);
                if (mainChart.options.scales.y) { mainChart.options.scales.y.min = Math.max(0, vMin - vPad); mainChart.options.scales.y.max = vMax + vPad; }
            }
            if (_vT.length > 0) {
                const tMin = Math.min.apply(null, _vT), tMax = Math.max.apply(null, _vT);
                const tPad = Math.max(2, (tMax - tMin) * 0.3);
                if (mainChart.options.scales.y1) { mainChart.options.scales.y1.min = Math.max(0, tMin - tPad); mainChart.options.scales.y1.max = tMax + tPad; }
            }
        }
    }

    if (mainChart) {
        mainChart.data.labels = chartLabels.slice();
        mainChart.data.datasets[0].data = chartVoltData.slice();
        mainChart.data.datasets[1].data = chartTempData.slice();
        mainChart.data.datasets[2].data = chartCurrData.slice();       // 修复: 电流(此前误赋 chartOfflineData → 切换后电流线消失)
        mainChart.data.datasets[3].data = chartOfflineData.slice();    // 离线指示线(此前漏赋)
        // 调整 X 轴显示刻度数(避免 150/1800 个刻度挤在一起)
        if (mainChart.options && mainChart.options.scales && mainChart.options.scales.x) {
            mainChart.options.scales.x.ticks.maxTicksLimit = cfg.maxTicks;
            mainChart.options.scales.x.ticks.autoSkip = true;
            mainChart.options.scales.x.ticks.autoSkipPadding = 16;
        }
        mainChart.update("none");
        // 2026-08-11: 切回/刷新进入实时范围时, 立即用最近历史预填充滚动缓冲,
        //   避免曲线空白或逐点生长(刷新后即时有数据)
        seedMainChartRealtime(range);
    }
    addLog("[视图] 实时曲线切换为 " + cfg.label + " (" + maxPoints + " 点)", "log-info");
    addOperationLog("实时曲线范围切换: " + cfg.label, "info");
}

// ============================================================
// 实时曲线图(Chart.js, 双轴)
// ============================================================
/* ====== 2026-08-09: 主曲线历史查询模式(6h/24h/7d) ======
 * 从 /api/history 拉取 pack_v/current/temp_max 历史数据绘制到主曲线,
 * 与 16 路曲线(loadCellVoltHistory)一样具备"记忆/可查询"能力:
 *   - 跨天/跨月自动加日期标签(fmtAxisSmartTime)
 *   - 离线段(相邻记录 >60s)用上一条电压画灰色虚线(gapData) */
function loadMainChartHistory(range, el, showLoading) {
    if (showLoading) _chartSetLoading(mainChart, true);
    if (el) {
        const container = el.closest(".chart-actions") || el.parentElement;
        if (container) container.querySelectorAll(".chart-tab").forEach(b => b.classList.remove("active"));
        el.classList.add("active");
    }
    const minMap = { "1h": 60, "6h": 360, "24h": 1440, "7d": 10080 };
    const minutes = minMap[range] || 60;
    // 2026-08-20 修复(#4): 同 loadCellVoltHistory, 7d/24h limit 提高覆盖全范围
    const limit = range === "7d" ? 100000 : (range === "24h" ? 50000 : 3000);
    // 2026-08-11: 仅用户触发的加载(showLoading)才接管中断控制器,
    //   避免离线补传/定时刷新(无 showLoading)误 abort 掉用户正在进行的查询, 也避免加载态卡死.
    let abortSignal = null;
    if (showLoading) {
        if (_mainChartHistoryAbort) { _mainChartHistoryAbort.abort(); }
        _mainChartHistoryAbort = new AbortController();
        abortSignal = _mainChartHistoryAbort.signal;
    }
    fetch("/api/history?minutes=" + minutes + "&limit=" + limit + "&agg=1", abortSignal ? { signal: abortSignal } : undefined)
        .then(r => r.json())
        .then(res => {
            if (!mainChart) return;
            const rows = res.data || [];
            // ===== 2026-08-10: 按范围单位聚合平均(替代逐条绘制) =====
            //   7d 等长范围下原始记录可达数千条, 直接画点挤成一团且 X 轴单位不随范围切换;
            //   现按范围选桶取平均: 1h=60点/分钟, 6h=36点/10分钟, 24h=24点/小时, 7d=168点/小时
            const agg = (res && res.agg) ? res.agg : _aggregateHistoryRows(rows, range);
            const labels = agg.labels, volt = agg.volt, temp = agg.temp, curr = agg.curr, off = agg.off;
            // 2026-08-09 修复: initMainChart 数据集为 4 组——
            //   [0]=电压 [1]=温度 [2]=电流 [3]=无数据(离线线)
            //   此前误把离线线写入 datasets[2](电流), 电流被覆盖/离线线未更新 → 历史查不到.
            mainChart.data.labels = labels;
            if (mainChart.data.datasets[0]) mainChart.data.datasets[0].data = volt;
            if (mainChart.data.datasets[1]) mainChart.data.datasets[1].data = temp;
            if (mainChart.data.datasets[2]) mainChart.data.datasets[2].data = curr;
            if (mainChart.data.datasets[3]) mainChart.data.datasets[3].data = off;
            if (mainChart.options && mainChart.options.scales && mainChart.options.scales.x && mainChart.options.scales.x.ticks) {
                // 2026-08-10: X 轴刻度上限随范围切换(7d 按天稀疏, 小时范围按分钟)
                mainChart.options.scales.x.ticks.maxTicksLimit = HIST_MAXTICKS[range] || 12;
                mainChart.options.scales.x.ticks.autoSkip = true;
            }
            // ===== 2026-08-10 坐标优化: 历史模式动态 Y 轴 =====
            //   与实时模式(pushMainChart)一致, 按聚合数据范围自适应,
            //   避免固定 0~30V / 0~60℃ 导致曲线被压扁或悬空
            try {
                const _clean = (arr) => arr.filter(v => v !== null && !isNaN(v) && v !== 0 && v !== "");
                const vv = _clean(volt), tt = _clean(temp), cc = _clean(curr);
                if (vv.length > 1) {
                    const vMin = Math.min.apply(null, vv), vMax = Math.max.apply(null, vv);
                    const vPad = Math.max(0.5, (vMax - vMin) * 0.15);
                    mainChart.options.scales.y.min = Math.max(0, vMin - vPad);
                    mainChart.options.scales.y.max = vMax + vPad;
                }
                if (tt.length > 1) {
                    const tMin = Math.min.apply(null, tt), tMax = Math.max.apply(null, tt);
                    const tPad = Math.max(1, (tMax - tMin) * 0.15);
                    mainChart.options.scales.y1.min = Math.max(0, tMin - tPad);
                    mainChart.options.scales.y1.max = tMax + tPad;
                }
                if (cc.length > 1) {
                    const cMin = Math.min.apply(null, cc), cMax = Math.max.apply(null, cc);
                    const cPad = Math.max(0.3, (cMax - cMin) * 0.15);
                    mainChart.options.scales.y2.min = cMin - cPad;
                    mainChart.options.scales.y2.max = cMax + cPad;
                }
            } catch (e) { /* Y 轴自适应失败不影响渲染 */ }
            // 2026-08-11 跨天统计日期: 直接使用聚合结果返回的跨天点, 7d 标签全带 MM-DD 后避免误判
            _applyDayMarkers(mainChart, agg.dayMarkers);
            mainChart.update("none");
            if (showLoading) _chartSetLoading(mainChart, false);
            addLog("[视图] 主曲线历史查询: " + range + " (" + rows.length + " 条 → " + labels.length + " 点)", "log-info");
        })
        .catch(err => {
            // 主动 abort 时不报错误; 其他异常输出日志便于排查。
            if (err && err.name === "AbortError") return;
            if (showLoading) _chartSetLoading(mainChart, false);
            console.warn("[视图] 主曲线历史查询失败:", err);
        });
}

// 2026-08-11: 实时曲线(1min/5min 滚动窗口)刷新后空白修复——
//   实时模式的滚动缓冲(chartVoltData 等)在页面加载时是空的, 只能靠实时帧逐点填充,
//   刷新后曲线要几十秒才"长"出来. 此函数用最近历史预填充滚动缓冲, 刷新后即时有数据.
function seedMainChartRealtime(range) {
    const cfg = {
        "1min": { points: 30,  minutes: 1, maxTicks: 8,  label: "1 分钟" },
        "5min": { points: 150, minutes: 5, maxTicks: 10, label: "5 分钟" },
    }[range];
    if (!cfg || !mainChart) return;
    fetch("/api/history?minutes=" + cfg.minutes + "&limit=" + cfg.points)
        .then(r => r.json())
        .then(res => {
            if (!mainChart) return;
            const rows = res.data || [];
            const slice = rows.slice(-cfg.points);   // 取最近 points 条(后端 ASC)
            // 修复: 历史为空时保留即时填充数据(切范围时已用 lastPayload 填满),
            //   不再清空为全 null, 否则会出现"切完范围曲线又变空白"的回退
            if (!slice.length) return;
            // 修复(2026-08-13-2): 历史数据不足窗口长度时,保留 setMainChartRange 已填充的
            //   flat 线(左侧),只覆盖右侧对应真实历史数据的位置。原逻辑是清空缓冲→推入
            //   slice→左侧补 null,导致切 5min 后只有最右侧一小段曲线,大片空白。
            const offset = cfg.points - slice.length;
            slice.forEach((row, i) => {
                const cells = row.cells || [];
                let v, t;
                if (selectedCellIndex >= 0 && cells[selectedCellIndex] != null) {
                    v = cells[selectedCellIndex] / 1000;
                    t = (Number(row.temp_max || 0) + Number(row.temp_min || 0)) / 2 / 10;
                } else {
                    v = Number(row.pack_v || 0) / 1000;
                    t = Number(row.temp_max || 0) / 10;
                }
                const idx = offset + i;
                chartVoltData[idx] = v > 0 ? v : null;
                chartTempData[idx] = t;
                chartCurrData[idx] = Number(row.current || 0) / 1000;
                chartOfflineData[idx] = null;
            });
            _curRangeCfg = { points: cfg.points, stepSec: 2, unit: "s", maxTicks: cfg.maxTicks, label: cfg.label };
            _regenChartLabels(Date.now());
            while (chartLabels.length > chartVoltData.length) chartLabels.shift();
            while (chartLabels.length < chartVoltData.length) chartLabels.unshift("");
            mainChart.data.labels = chartLabels.slice();
            mainChart.data.datasets[0].data = chartVoltData.slice();
            mainChart.data.datasets[1].data = chartTempData.slice();
            mainChart.data.datasets[2].data = chartCurrData.slice();
            mainChart.data.datasets[3].data = chartOfflineData.slice();
            if (mainChart.options && mainChart.options.scales && mainChart.options.scales.x && mainChart.options.scales.x.ticks) {
                mainChart.options.scales.x.ticks.maxTicksLimit = cfg.maxTicks;
                mainChart.options.scales.x.ticks.autoSkip = true;
                mainChart.options.scales.x.ticks.autoSkipPadding = 16;
            }
            mainChart.update("none");
            if (slice.length) addLog("[视图] 实时曲线已从历史预填充 " + slice.length + " 点 (" + cfg.label + ")", "log-info");
        })
        .catch(() => {});
}

// 2026-08-11: 刷新后恢复用户上次选择的主曲线时间范围(含 1h/6h/24h/7d 历史范围),
//   避免刷新后实时曲线空白、或历史范围被重置回默认 1 分钟.
function initMainChartRange() {
    let range = "1min";
    try {
        const saved = localStorage.getItem("bms_main_range");
        if (saved) range = saved;
    } catch (e) {}
    let el = null;
    const host = document.getElementById("mainChart");
    if (host) {
        const card = host.closest(".chart-card");
        if (card) {
            card.querySelectorAll(".chart-actions .chart-tab").forEach(b => {
                const oc = b.getAttribute("onclick") || "";
                if (oc.indexOf("'" + range + "'") >= 0) el = b;
            });
        }
    }
    // setMainChartRange 内部已按 range 处理: 历史范围→loadMainChartHistory(恢复历史曲线);
    //   实时范围→重建缓冲 + seedMainChartRealtime(从历史预填充, 避免空白)
    if (el) setMainChartRange(range, el);
    else setMainChartRange(range, null);
}

function initMainChart() {
    const ctx = $("mainChart");
    if (!ctx) return;
    const c = ctx.getContext("2d");
    mainChart = new Chart(c, {
        type: "line",
        data: {
            labels: chartLabels.slice(),
            datasets: [
                {
                    label: "总电压 (V)",
                    data: chartVoltData.slice(),
                    borderColor: COLOR.cyan,
                    backgroundColor: "rgba(6,182,212,0.08)",
                    borderWidth: 3,
                    fill: true,
                    tension: 0.4,
                    pointRadius: 0,
                    yAxisID: "y",
                },
                {
                    label: "最高温度 (°C)",
                    data: chartTempData.slice(),
                    borderColor: COLOR.yellow,
                    backgroundColor: "rgba(245,158,11,0.05)",
                    borderWidth: 2.5,
                    fill: true,
                    tension: 0.4,
                    pointRadius: 0,
                    yAxisID: "y1",
                },
                {
                    label: "电流 (A)",
                    data: chartCurrData.slice(),
                    borderColor: COLOR.green,
                    backgroundColor: "rgba(16,185,129,0.05)",
                    borderWidth: 2.5,
                    fill: true,
                    tension: 0.4,
                    pointRadius: 0,
                    yAxisID: "y2",
                },
                {
                    label: "无数据",
                    data: chartOfflineData.slice(),
                    borderColor: "#666",
                    borderWidth: 1,
                    borderDash: [4, 4],     // 灰色虚线表示断线区间
                    fill: false,
                    tension: 0,
                    pointRadius: 0,
                    yAxisID: "y",
                    spanGaps: true,         // 跨越 null 连接, 形成连续虚线
                },
            ],
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: { duration: 150 },   // 优化: 300ms→150ms 减少频繁更新时的卡顿
            interaction: { mode: "index", intersect: false },
            plugins: {
                decimation: { enabled: true, algorithm: "min-max", samples: 200 },
                legend: { labels: { color: COLOR.textSec, font: { size: 11 }, boxWidth: 12, padding: 16 } },
                // ===== 2026-08-10 修复: 显式启用 tooltip, 悬停显示所有通道数值 =====
                //   原配置仅 legend, tooltip 依赖默认值且被 CrosshairPlugin 重绘干扰,
                //   导致"鼠标放上去看不到数据". 此处显式配置样式 + 数值格式化.
                tooltip: {
                    enabled: true,
                    mode: "index",
                    intersect: false,
                    backgroundColor: "rgba(15,23,42,0.92)",
                    titleColor: "#e2e8f0",
                    bodyColor: "#cbd5e1",
                    borderColor: "rgba(100,116,139,0.45)",
                    borderWidth: 1,
                    padding: 8,
                    cornerRadius: 6,
                    displayColors: true,
                    callbacks: {
                        label: (item) => {
                            const v = item.raw;
                            if (v === null || v === undefined || v === "") return item.dataset.label + ": --";
                            return item.dataset.label + ": " + Number(v).toFixed(2);
                        },
                    },
                },
            },
            scales: {
                x: {
                    grid: { color: themeGridColor() },
                    ticks: {
                        color: COLOR.textMut,
                        font: { size: 10 },
                        maxTicksLimit: 8,
                        autoSkip: true,
                        autoSkipPadding: 16,
                        maxRotation: 0,
                        minRotation: 0,
                        // ===== 2026-08-10 坐标优化: 时间刻度智能显示 =====
                        //   1min 保留秒级(HH:MM:SS); 5min/1h/6h 长范围去秒(HH:MM)防重叠
                        callback: function(value) {
                            let lb = "";
                            try { lb = this.getLabelForValue(value) || ""; } catch (e) {}
                            if (!lb) return lb;
                            const m = /^(\d{2}:\d{2}):\d{2}$/.exec(lb);
                            if (m && (mainChartRange === "5min" || mainChartRange === "1h" || mainChartRange === "6h")) {
                                return m[1];
                            }
                            return lb;
                        },
                    },
                },
                y: {
                    position: "left",
                    grid: { color: themeGridColor() },
                    ticks: { color: COLOR.cyan, font: { size: 10 } },
                    title: { display: true, text: "电压 (V)", color: COLOR.cyan, font: { size: 10 } },
                    suggestedMin: 0,
                    suggestedMax: 30,
                },
                y1: {
                    position: "right",
                    grid: { drawOnChartArea: false },
                    ticks: { color: COLOR.yellow, font: { size: 10 } },
                    title: { display: true, text: "温度 (°C)", color: COLOR.yellow, font: { size: 10 } },
                    suggestedMin: 0,
                    suggestedMax: 60,
                },
                y2: {
                    position: "right",
                    grid: { drawOnChartArea: false },
                    ticks: { color: COLOR.green, font: { size: 10 } },
                    title: { display: true, text: "电流 (A)", color: COLOR.green, font: { size: 10 } },
                    suggestedMin: -10,
                    suggestedMax: 10,
                },
            },
        },
    });
}

// 重新生成实时曲线 X 轴标签(最右=最新数据时刻, 向左按 stepSec 递减)
// 2026-08-08: 横坐标由相对偏移("-30s"/"now")改为具体时刻 HH:MM:SS
function _fmtClock(ms) {
    const d = new Date(ms);
    const pad = (n) => String(n).padStart(2, "0");
    return pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
}

function _regenChartLabels(baseMs) {
    const cfg = _curRangeCfg || { points: 30, stepSec: 2 };
    const n = chartVoltData.length;
    if (n === 0) return;
    if (!(baseMs > 946684800000)) baseMs = Date.now();   // 无效时间戳回退为当前时间
    chartLabels.length = 0;
    for (let i = n - 1; i >= 0; i--) {
        chartLabels.push(_fmtClock(baseMs - i * cfg.stepSec * 1000));
    }
}

function pushMainChart(p) {
    // 2026-08-09 修复: 历史查询模式(6h/24h/7d)下暂停实时推送,
    //   避免每帧覆盖已渲染的历史数据("历史图没了"); 切回实时范围恢复
    if (_mainChartHistMode) return;
    const OFFLINE_MS = 10 * 1000;   // 2026-08-21: 20s→10s(用户要求 10s 内判定离线)
    let ageMs = Number(window.__devAgeMs || 0);
    if (ageMs <= 0 && typeof p.last_event_time_ms === "number" && p.last_event_time_ms > 946684800000) {
        ageMs = Math.max(0, Date.now() - p.last_event_time_ms);
    }
    // 横坐标时刻基准: 优先用数据上报时刻, 无效时用当前时间
    // 2026-08-08 修复: 设备时间超前(快几分钟)时, 用浏览器当前时间兜底,
    //   否则曲线横坐标比真实时间快, 用户看到"时间超前"
    let baseMs = Date.now();
    if (typeof p.last_event_time_ms === "number" && p.last_event_time_ms > 946684800000) {
        baseMs = Math.min(p.last_event_time_ms, Date.now() + 5000);   // 钳制: 不允许超前浏览器时间超过5s
    }
    // no_data: 主数据线 push null 形成断线, 离线指示线 push 最后有效值形成灰色虚线
    if (p.no_data) {
        chartVoltData.shift();
        chartVoltData.push(null);
        chartTempData.shift();
        chartTempData.push(null);
        chartCurrData.shift();
        chartCurrData.push(null);
        chartOfflineData.shift();
        chartOfflineData.push(lastValidVoltV > 0 ? lastValidVoltV : null);  // 灰色虚线保持最后有效电压
        _regenChartLabels(baseMs);   // 重新生成标签(修复: 旧代码标签全变"now")
        if (mainChart) {
            mainChart.data.labels = chartLabels.slice();
            mainChart.data.datasets[0].data = chartVoltData.slice();
            mainChart.data.datasets[1].data = chartTempData.slice();
            mainChart.data.datasets[2].data = chartCurrData.slice();
            mainChart.data.datasets[3].data = chartOfflineData.slice();
            mainChart.update("none");
        }
        return;
    }
    // 数据陈旧 (>60s): 不追加新点, 主数据线 push null 形成断线, 灰色虚线保持最后有效电压
    // 2026-08-19 修复(C4): 原实现只 update("none") 不画灰线, 断线表现与 no_data 分支不一致
    if (ageMs > OFFLINE_MS) {
        chartVoltData.shift();   chartVoltData.push(null);
        chartTempData.shift();   chartTempData.push(null);
        chartCurrData.shift();   chartCurrData.push(null);
        chartOfflineData.shift();
        chartOfflineData.push(lastValidVoltV > 0 ? lastValidVoltV : null);  // 灰色虚线保持最后有效电压
        if (mainChart) {
            mainChart.data.datasets[0].data = chartVoltData.slice();
            mainChart.data.datasets[1].data = chartTempData.slice();
            mainChart.data.datasets[2].data = chartCurrData.slice();
            mainChart.data.datasets[3].data = chartOfflineData.slice();
            mainChart.update("none");
        }
        return;
    }
    let voltV, tempC;
    const cells = p.cells || [];
    if (selectedCellIndex >= 0 && cells[selectedCellIndex] != null) {
        // 选中单体: 显示该单体电压和平均温度
        voltV = cells[selectedCellIndex] / 1000;
        tempC = (Number(p.temp_max || 0) + Number(p.temp_min || 0)) / 2 / 10;
    } else {
        // Bug4 修复: 默认显示总电压(不是单体最高电压)
        voltV = Number(p.pack_v || 0) / 1000;
        tempC = Number(p.temp_max || 0) / 10;
    }
    // 记录最后有效电压值(用于断线时保持水平虚线)
    if (voltV > 0) lastValidVoltV = voltV;
    chartVoltData.shift();
    chartVoltData.push(voltV);
    chartTempData.shift();
    chartTempData.push(tempC);
    chartCurrData.shift();
    chartCurrData.push(Number(p.current || 0) / 1000);
    chartOfflineData.shift();
    chartOfflineData.push(null);   // 正常数据时离线指示线为 null
    // 重新生成 X 轴标签: 最右=最新数据时刻, 向左按 stepSec 递减
    // 修复: 旧代码只 shift+push("now") 导致所有标签逐渐变成 "now", 看不到时间刻度
    _regenChartLabels(baseMs);
    if (mainChart) {
        mainChart.data.labels = chartLabels.slice();
        mainChart.data.datasets[0].data = chartVoltData.slice();
        mainChart.data.datasets[1].data = chartTempData.slice();
        mainChart.data.datasets[2].data = chartCurrData.slice();
        mainChart.data.datasets[3].data = chartOfflineData.slice();
        // ===== 动态调整Y轴范围(空数据保护) =====
        const validV = chartVoltData.filter(v => v !== null && !isNaN(v) && v > 0);
        const validT = chartTempData.filter(v => v !== null && !isNaN(v) && v > 0);
        const validC = chartCurrData.filter(v => v !== null && !isNaN(v) && v !== 0);
        if (validV.length > 0) {
            const vMin = Math.min(...validV);
            const vMax = Math.max(...validV);
            const vPad = Math.max(0.5, (vMax - vMin) * 0.3);
            mainChart.options.scales.y.min = Math.max(0, vMin - vPad);
            mainChart.options.scales.y.max = vMax + vPad;
        }
        if (validT.length > 0) {
            const tMin = Math.min(...validT);
            const tMax = Math.max(...validT);
            const tPad = Math.max(2, (tMax - tMin) * 0.3);
            mainChart.options.scales.y1.min = Math.max(0, tMin - tPad);
            mainChart.options.scales.y1.max = tMax + tPad;
        }
        if (validC.length > 0) {
            const cMin = Math.min(...validC);
            const cMax = Math.max(...validC);
            const cPad = Math.max(1, (cMax - cMin) * 0.3);
            mainChart.options.scales.y2.min = cMin - cPad;
            mainChart.options.scales.y2.max = cMax + cPad;
        }
        mainChart.update("none");
    }
}

// ============================================================
// 历史趋势图(3 轴: 电压/电流/功率)
// ============================================================
function initHistoryChart() {
    const ctx = $("historyChart");
    if (!ctx) return;
    const c = ctx.getContext("2d");
    historyChart = new Chart(c, {
        type: "line",
        data: {
            labels: [],
            datasets: [
                { label: "总电压 (V)", data: [], borderColor: COLOR.blue, borderWidth: 1.5, tension: 0.4, pointRadius: 0, yAxisID: "y" },
                { label: "总电流 (A)", data: [], borderColor: COLOR.green, borderWidth: 1.5, tension: 0.4, pointRadius: 0, yAxisID: "y1" },
                { label: "功率 (W)", data: [], borderColor: COLOR.yellow, borderWidth: 1, tension: 0.4, pointRadius: 0, yAxisID: "y2", borderDash: [3, 3] },
                // 2026-08-08: 无数据区段指示线(灰色虚线, 显示设备离线/断传的时间段)
                { label: "无数据区段", data: [], borderColor: "#8a8f98", borderWidth: 1, borderDash: [6, 4], tension: 0, pointRadius: 0, yAxisID: "y", spanGaps: true },
            ],
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            // 2026-08-11: 关闭动画——历史数据每次刷新/悬停都不再有 300ms 过渡, 交互即时跟手
            animation: false,
            interaction: { mode: "index", intersect: false },
            plugins: {
                legend: { labels: { color: COLOR.textSec, font: { size: 11 }, boxWidth: 12, padding: 10, usePointStyle: true } },
                // 2026-08-11: 悬停逐点信息(时间 + 各通道数值 + 单位; 无数据显"无数据")
                tooltip: {
                    mode: "index",
                    intersect: false,
                    backgroundColor: "rgba(15,23,42,0.95)",
                    titleColor: "#e2e8f0",
                    bodyColor: "#e2e8f0",
                    borderColor: "rgba(148,163,184,0.4)",
                    borderWidth: 1,
                    padding: 10,
                    callbacks: {
                        title: function (items) {
                            if (!items || !items.length) return "";
                            return "时间 " + items[0].label;   // 标签已是 HH:MM / MM-DD HH:00
                        },
                        label: function (ctx) {
                            const v = ctx.parsed.y;
                            const name = ctx.dataset.label || "";
                            if (v == null || isNaN(v)) return name + ": 无数据";
                            let unit = "";
                            if (name.indexOf("电压") >= 0) unit = " V";
                            else if (name.indexOf("电流") >= 0) unit = " A";
                            else if (name.indexOf("功率") >= 0) unit = " W";
                            return name + ": " + v + unit;
                        }
                    }
                }
            },
            scales: {
                x: { grid: { color: themeGridColor() }, ticks: { color: themeMutedColor(), font: { size: 10 }, maxTicksLimit: 10 } },
                y:  { type: "linear", display: true, position: "left",   grid: { color: themeGridColor() }, ticks: { color: COLOR.blue,   font: { size: 10 } }, title: { display: true, text: "电压(V)", color: COLOR.blue } },
                y1: { type: "linear", display: true, position: "right",  grid: { drawOnChartArea: false }, ticks: { color: COLOR.green,  font: { size: 10 } }, title: { display: true, text: "电流(A)", color: COLOR.green } },
                y2: { type: "linear", display: true, position: "right",  grid: { drawOnChartArea: false }, ticks: { color: COLOR.yellow, font: { size: 10 } }, title: { display: true, text: "功率(W)", color: COLOR.yellow } },
            },
        },
    });
}

let _historyRangeTimer = null;
let _historyHistoryAbort = null;
function setHistoryRange(range, el) {
    historyRange = range;
    // 仅切换当前 chart-actions 内的 tab
    const tabs = el ? el.parentElement.querySelectorAll(".chart-tab") : document.querySelectorAll('button.chart-tab');
    tabs.forEach(b => b.classList.remove("active"));
    if (el) el.classList.add("active");
    // 2026-08-11: 用户点击范围按钮 → 立即加载态(感知速度) + 防抖 + 中断旧请求,
    //   避免快速切换 1h/6h/24h/7d 时堆叠多次大查询拖慢页面.
    _chartSetLoading(historyChart, true);
    const eh = $("historyEmptyHint"); if (eh) eh.style.display = "none";
    if (_historyRangeTimer) clearTimeout(_historyRangeTimer);
    if (_historyHistoryAbort) { _historyHistoryAbort.abort(); _historyHistoryAbort = null; }
    _historyRangeTimer = setTimeout(() => {
        _historyRangeTimer = null;
        loadHistoryChart(true);
    }, 200);
    addOperationLog("历史范围切换: " + range, "info");
}

function loadHistoryChart(showLoading) {
    // 范围映射为 minutes
    const range = historyRange || "1h";
    const minMap = { "1h": 60, "6h": 360, "24h": 1440, "7d": 10080 };
    const minutes = minMap[range] || 60;
    // 2026-08-20 修复(#4): 同 loadCellVoltHistory, 7d/24h limit 提高覆盖全范围
    const limit = range === "7d" ? 100000 : (range === "24h" ? 50000 : 3000);
    // 2026-08-11: 仅用户触发的加载(showLoading)才接管中断控制器,
    //   避免 60s 定时刷新/补传刷新误 abort 掉用户正在进行的查询.
    let _signal = null;
    if (showLoading) {
        if (_historyHistoryAbort) _historyHistoryAbort.abort();
        _historyHistoryAbort = new AbortController();
        _signal = _historyHistoryAbort.signal;
    }
    // 2026-08-11: 优先请求服务端聚合(agg=1)——7d 仅回传 ~168 点而非 1 万行, 根治"反应慢";
    //   旧后端不支持 agg 时 res.agg 为空, 前端兜底用 _aggregateHistoryRows(逻辑一致).
    fetch("/api/history?minutes=" + minutes + "&limit=" + limit + "&agg=1", _signal ? { signal: _signal } : undefined)
        .then(r => r.json())
        .then(res => {
            if (!historyChart) return;
            const rows = res.data || [];
            const agg = (res && res.agg) ? res.agg : _aggregateHistoryRows(rows, range);
            const labels = agg.labels;
            const voltData = agg.volt;
            const currData = agg.curr;
            const pwrData = voltData.map((v, i) => {
                const a = currData[i];
                return (v != null && a != null) ? Number((v * a).toFixed(1)) : null;
            });
            historyChart.data.labels = labels;
            historyChart.data.datasets[0].data = voltData;   // 总电压 (V)
            historyChart.data.datasets[1].data = currData;   // 总电流 (A)
            historyChart.data.datasets[2].data = pwrData;    // 功率 (W)
            historyChart.data.datasets[3].data = agg.off;    // 无数据区段
            // X 轴刻度上限随范围切换(7d 按天稀疏, 小时范围按分钟)
            if (historyChart.options && historyChart.options.scales && historyChart.options.scales.x && historyChart.options.scales.x.ticks) {
                historyChart.options.scales.x.ticks.maxTicksLimit = HIST_MAXTICKS[range] || 12;
            }
            // ===== 2026-08-11 坐标轴自适应: 有数据放大, 无数据给可见框架(根治"坐标消失") =====
            const y = historyChart.options.scales.y;
            const y1 = historyChart.options.scales.y1;
            const y2 = historyChart.options.scales.y2;
            const validV = voltData.filter(v => v != null && v > 0);
            const validA = currData.filter(v => v != null && v !== 0);
            const validP = pwrData.filter(v => v != null);
            if (validV.length > 0) {
                const vMin = Math.min.apply(null, validV);
                const vMax = Math.max.apply(null, validV);
                const vPad = Math.max(0.5, (vMax - vMin) * 0.15);
                _axisZoom(y, Math.max(0, vMin - vPad), vMax + vPad);
            } else {
                _axisFallback(y, 0, 60);   // 12/24/48/60V 系统均覆盖的可见电压轴
            }
            if (validA.length > 0) {
                const aMin = Math.min.apply(null, validA);
                const aMax = Math.max.apply(null, validA);
                const aPad = Math.max(0.5, (aMax - aMin) * 0.15);
                _axisZoom(y1, aMin - aPad, aMax + aPad);
            } else {
                _axisFallback(y1, -50, 50);
            }
            if (validP.length > 0) {
                const pMin = Math.min.apply(null, validP);
                const pMax = Math.max.apply(null, validP);
                const pPad = Math.max(10, (pMax - pMin) * 0.15);
                _axisZoom(y2, pMin - pPad, pMax + pPad);
            } else {
                _axisFallback(y2, -500, 1500);
            }
            historyChart.update("none");
            if (showLoading) _chartSetLoading(historyChart, false);
            // 2026-08-11 跨天日期分隔标线: 使用聚合结果返回的跨天点
            try { _applyDayMarkers(historyChart, agg.dayMarkers); } catch (e) {}
            // ===== 空/缺数据提示: 准确告知设备数据缺口, 不再误导"重启 Dashboard" =====
            const emptyHint = $("historyEmptyHint");
            if (emptyHint) {
                if (validV.length > 0) {
                    emptyHint.style.display = "none";
                } else {
                    // 设备最后一条有效电压时间(服务端聚合时由后端全局查询; 前端聚合时从桶里推)
                    let lastV = (agg && agg.last_voltage_ts) ? agg.last_voltage_ts : 0;
                    if (!lastV && agg && agg.bucketTs && agg.volt) {
                        for (let i = 0; i < agg.bucketTs.length; i++) {
                            if (agg.volt[i] != null && agg.volt[i] > 0) lastV = Math.max(lastV, agg.bucketTs[i]);
                        }
                    }
                    let msg = "该时间段无电压历史数据";
                    if (lastV > 0) {
                        const d = new Date(lastV * 1000);
                        const p2 = n => String(n).padStart(2, "0");
                        const fmt = d.getFullYear() + "-" + p2(d.getMonth() + 1) + "-" + p2(d.getDate()) +
                                    " " + p2(d.getHours()) + ":" + p2(d.getMinutes());
                        msg += "(设备自 " + fmt + " 起停止上报电压, 仅电流仍在记录); 曲线已显示电流";
                    } else {
                        msg += "(设备从未上报电压, 请检查采样/接线)";
                    }
                    emptyHint.textContent = msg;
                    emptyHint.style.display = "block";
                }
            }
            addLog("[历史] 加载 " + (res && res.agg ? "服务端聚合" : rows.length + " 条") + " (" + range + ", " + labels.length + " 点)", "log-success");
        })
        .catch(err => {
            // 主动 abort(快速切换)时不报错误, 也不撤掉加载态(下一个请求会接管)
            if (err && err.name === "AbortError") return;
            if (showLoading) _chartSetLoading(historyChart, false);
            addLog("[历史] 加载失败: " + err, "log-error");
        });
}

// ============================================================
// SOC/SOH 30 天趋势图
// ============================================================
function initTrendChart() {
    const ctx = $("trendChart");
    if (!ctx) return;
    const c = ctx.getContext("2d");
    // 先用占位数据初始化,后续从 /api/history 聚合
    const days = [], socTrend = [], sohTrend = [];
    for (let i = 29; i >= 0; i--) {
        const d = new Date(Date.now() - i * 86400000);
        days.push((d.getMonth() + 1) + "/" + d.getDate());
        socTrend.push(null);
        sohTrend.push(null);
    }
    trendChart = new Chart(c, {
        type: "line",
        data: {
            labels: days,
            datasets: [
                { label: "SOC (%)", data: socTrend, borderColor: COLOR.blue,   backgroundColor: "rgba(59,130,246,0.08)",  borderWidth: 1.5, fill: true, tension: 0.4, pointRadius: 0 },
                { label: "SOH (%)", data: sohTrend, borderColor: COLOR.purple, backgroundColor: "rgba(139,92,246,0.05)", borderWidth: 1.5, fill: true, tension: 0.4, pointRadius: 0 },
            ],
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: { duration: 500 },
            interaction: { mode: "index", intersect: false },
            plugins: {
                legend: { labels: { color: COLOR.textSec, font: { size: 11 }, boxWidth: 12, padding: 12, usePointStyle: true } },
            },
            scales: {
                x: { grid: { color: themeGridColor() }, ticks: { color: themeMutedColor(), font: { size: 10 }, maxTicksLimit: 10 } },
                y: { min: 0, max: 100, grid: { color: themeGridColor() }, ticks: { color: "#94a3b8", font: { size: 10 } } },
            },
        },
    });
}

/**
 * P2-5: SOC/SOH 趋势曲线 (右上角按钮 日/周/月)
 * @param {string} range  day=24h 按小时 | week=7d 按天 | month=30d 按天
 * @param {HTMLElement} el 被点击的按钮 (用于切换 active)
 */
let lastTrendRange = "month";
let _trendRangeTimer = null;
let _trendAbort = null;
function loadTrendChart(range, el) {
    range = range || "month";
    lastTrendRange = range;
    // 2026-08-11: 用户点击范围按钮(el!=null) → 防抖 + 中断旧请求 + 加载态(感知速度);
    //   定时刷新/初始化(el 为空)直接加载, 不打断用户查询也不弹加载态.
    if (el != null) {
        _chartSetLoading(trendChart, true);
        if (_trendRangeTimer) clearTimeout(_trendRangeTimer);
        if (_trendAbort) { _trendAbort.abort(); _trendAbort = null; }
        _trendAbort = new AbortController();
        const _sig = _trendAbort.signal;
        _trendRangeTimer = setTimeout(() => {
            _trendRangeTimer = null;
            _loadTrendChartCore(range, el, _sig, true);
        }, 300);
        return;
    }
    _loadTrendChartCore(range, el, null, false);
}

function _loadTrendChartCore(range, el, signal, showLoading) {
    // 修复: el 为空(初始化或定时刷新)时,按 range 自动找对应按钮补 active
    let btnEl = el;
    if (!btnEl) {
        const labelMap = { day: "日", week: "周", month: "月" };
        const want = labelMap[range] || "月";
        const host = document.getElementById("trendChart");
        if (host) {
            const card = host.closest(".chart-card");
            if (card) {
                const cand = card.querySelectorAll(".chart-actions .chart-tab");
                cand.forEach(b => {
                    const isMatch = (b.textContent.trim() === want) || (b.getAttribute("onclick") || "").indexOf("'" + range + "'") >= 0;
                    if (isMatch) btnEl = b;
                });
            }
        }
    }
    if (btnEl) {
        const container = btnEl.closest(".chart-actions");
        if (container) container.querySelectorAll(".chart-tab").forEach(b => b.classList.remove("active"));
        btnEl.classList.add("active");
    }
    // 时间窗口(分钟)
    const minMap = { day: 1440, week: 10080, month: 43200 };
    const minutes = minMap[range] || 43200;
    // 聚合粒度: day => 按小时; week/month => 按天
    const byHour = range === "day";
    // 2026-08-11: limit 200000 → 20000, 避免一次性拉全量库(可达数十 MB)导致趋势图卡死;
    //   当前库 ~1.2 万行, 20000 已覆盖 30 天窗口; 后端 agg 重启后本请求将改为服务端聚合.
    fetch("/api/history?minutes=" + minutes + "&limit=20000", signal ? { signal } : undefined)
        .then(r => r.json())
        .then(res => {
            if (!trendChart) return;
            let rows = res.data || [];
            // 2026-08-09 修复: 趋势图降采样——limit=200000 可能拉全量 20 万条,
            //   逐条聚合会导致页面卡顿. 超过 2000 条时均匀抽样(保趋势, 降开销).
            if (rows.length > 2000) {
                const step = rows.length / 2000;
                const sampled = [];
                for (let i = 0; i < 2000; i++) sampled.push(rows[Math.floor(i * step)]);
                rows = sampled;
                addLog("[趋势] 数据量大, 已降采样 " + (res.data || []).length + " → 2000 点", "log-info");
            }
            if (rows.length === 0) {
                const tip = $("trendChartTip");
                if (tip) tip.textContent = "暂无数据";
                if (showLoading) _chartSetLoading(trendChart, false);
                addLog("[趋势] " + (range === "day" ? "日" : range === "week" ? "周" : "30天") + " 无数据", "log-error");
                return;
            }
            const buckets = new Map();   // 保序
            rows.forEach(row => {
                const d = new Date(row.recv_time * 1000);
                let key;
                if (byHour) {
                    key = (d.getMonth() + 1) + "/" + d.getDate() + " " +
                        String(d.getHours()).padStart(2, "0") + ":00";
                } else {
                    const y = d.getFullYear();
                    const m = String(d.getMonth() + 1).padStart(2, "0");
                    const day = String(d.getDate()).padStart(2, "0");
                    key = y + "-" + m + "-" + day;
                }
                if (!buckets.has(key)) buckets.set(key, { soc: [], soh: [], _d: d });
                const b = buckets.get(key);
                b.soc.push(Number(row.soc || 0));
                b.soh.push(Number(row.soh || 0));
            });
            // 对 week/month: 按日期补全空桶,避免曲线错位
            let keys = [...buckets.keys()];
            if (!byHour && buckets.size > 1) {
                // 以首尾日期为界, 补出每一天
                const firstKey = keys[0], lastKey = keys[keys.length - 1];
                const [y1, m1, d1] = firstKey.split("-").map(Number);
                const [y2, m2, d2] = lastKey.split("-").map(Number);
                const start = new Date(y1, m1 - 1, d1);
                const end = new Date(y2, m2 - 1, d2);
                const all = [];
                for (let t = +start; t <= +end; t += 86400000) {
                    const dt = new Date(t);
                    const k = dt.getFullYear() + "-" + String(dt.getMonth() + 1).padStart(2, "0")
                        + "-" + String(dt.getDate()).padStart(2, "0");
                    all.push(k);
                    if (!buckets.has(k)) buckets.set(k, { soc: [], soh: [], _d: dt });
                }
                keys = all;
            }
            const labels = [], socArr = [], sohArr = [];
            keys.forEach(k => {
                const b = buckets.get(k);
                if (!b) return;
                if (b.soc.length === 0) {
                    labels.push(k);
                    socArr.push(null);
                    sohArr.push(null);
                    return;
                }
                const socAvg = b.soc.reduce((a, x) => a + x, 0) / b.soc.length;
                const sohAvg = b.soh.reduce((a, x) => a + x, 0) / b.soh.length;
                labels.push(k);
                socArr.push(Number(socAvg.toFixed(1)));
                sohArr.push(Number(sohAvg.toFixed(1)));
            });
            trendChart.data.labels = labels;
            trendChart.data.datasets[0].data = socArr;
            trendChart.data.datasets[1].data = sohArr;
            const allVals = socArr.concat(sohArr).filter(x => typeof x === "number" && !isNaN(x));
            trendChart.options.scales.y.min = allVals.length ? Math.max(0, Math.min(...allVals) - 5) : 0;
            trendChart.options.scales.y.max = 100;
            trendChart.update("none");
            if (showLoading) _chartSetLoading(trendChart, false);
            const tip = $("trendChartTip");
            if (tip) tip.textContent = "共 " + rows.length + " 条原始记录 · " + keys.length + " 个聚合点";
            addLog("[趋势] 范围 " + (range === "day" ? "24小时" : range === "week" ? "7天" : "30天")
                + " · 聚合点 " + keys.length, "log-success");
        })
        .catch(err => {
            // 主动 abort(快速切换)不撤加载态, 下一个请求会接管
            if (err && err.name === "AbortError") return;
            if (showLoading) _chartSetLoading(trendChart, false);
            console.warn("loadTrendChart fail:", err);
        });
}

// ============================================================
// 告警列表
// ============================================================
function renderAlertList(faultCode) {
    const list = $("alertList");
    if (!list) return;
    const alerts = parseFault(faultCode);
    // 合并: 保留未确认的旧告警, 加上新告警
    const existingIds = new Set(currentAlerts.filter(a => !a.acknowledged).map(a => a.code));
    alerts.forEach(a => {
        if (!existingIds.has(a.code)) {
            currentAlerts.unshift({
                id: ++alertIdSeq,
                code: a.code,
                level: a.level,
                desc: a.desc,
                time: fmtNow(),
                acknowledged: false,
            });
            // 2026-08-09: 新告警触发浏览器系统通知(需用户授权, 可开关)
            if (a.level === "fatal") {
                notifyBrowser("BMS 严重告警: " + a.desc, "故障码 0x" + a.code.toString(16).toUpperCase(), "🚨");
            } else {
                notifyBrowser("BMS 告警: " + a.desc, "故障码 0x" + a.code.toString(16).toUpperCase(), "⚠️");
            }
        }
    });
    // 移除已经不在故障码中的旧告警(已恢复)
    const currentCodes = new Set(alerts.map(a => a.code));
    currentAlerts = currentAlerts.filter(a => a.acknowledged || currentCodes.has(a.code) || a.persistent);
    // 限制 20 条
    if (currentAlerts.length > 20) currentAlerts = currentAlerts.slice(0, 20);

    let html = "";
    if (currentAlerts.length === 0) {
        html = '<div style="color:var(--text-muted);padding:20px;text-align:center">✓ 暂无告警</div>';
    } else {
        currentAlerts.forEach(a => {
            const cls = levelToClass(a.level);
            const icon = levelToIcon(a.level);
            const hex = "0x" + a.code.toString(16).toUpperCase().padStart(5, "0");
            const ackBtn = a.acknowledged
                ? '<span style="font-size:11px;color:var(--text-muted)">已确认</span>'
                : '<button class="alert-btn primary" onclick="ackAlert(' + a.id + ')">确认</button>';
            html += '<div class="alert-item ' + cls + '" data-id="' + a.id + '">' +
                    '<div class="alert-icon">' + icon + '</div>' +
                    '<div class="alert-body">' +
                    '<div class="alert-msg">' + htmlEsc(a.desc) + '</div>' +
                    '<div class="alert-meta"><span>故障码: ' + hex + '</span><span>🕐 ' + a.time + '</span></div>' +
                    '</div>' +
                    '<div class="alert-actions">' +
                    ackBtn +
                    '<button class="alert-btn" onclick="dismissAlert(' + a.id + ')">删除</button>' +
                    '</div>' +
                    '</div>';
        });
    }
    list.innerHTML = html;
    // 更新告警计数
    const activeCount = currentAlerts.filter(a => !a.acknowledged).length;
    if ($("alertCount")) $("alertCount").textContent = activeCount;
    if ($("alertBadge")) $("alertBadge").textContent = activeCount;
}

function ackAlert(id) {
    const a = currentAlerts.find(x => x.id === id);
    if (a) a.acknowledged = true;
    renderAlertList(lastPayload ? lastPayload.fault : 0);
    addLog("[告警] 已确认告警 ID=" + id, "log-info");
    addOperationLog("告警已确认: " + (a ? a.desc : ""), "success");
}

function dismissAlert(id) {
    currentAlerts = currentAlerts.filter(x => x.id !== id);
    renderAlertList(lastPayload ? lastPayload.fault : 0);
    addLog("[告警] 已忽略告警 ID=" + id, "log-info");
    addOperationLog("告警已忽略: ID=" + id, "info");
}

function ackAllAlerts() {
    currentAlerts.forEach(a => a.acknowledged = true);
    renderAlertList(lastPayload ? lastPayload.fault : 0);
    addOperationLog("已确认全部告警", "success");
}

// ============================================================
// 2026-08-13: 历史故障记录(对接后端 /api/faults, 刷新不丢失)
//   实时告警(currentAlerts) 仅靠 socket 推送, 刷新页面即清空;
//   本函数拉取 DB 中 fault!=0 的历史记录, 按故障码去重(保留最近一次上报),
//   渲染为只读"历史故障"列表, 与可确认/忽略的实时告警区分.
// ============================================================
let _faultHistoryLoaded = false;
let _faultHistoryItems = [];
let _hiddenFaultCodes = new Set();

// 通用 CSV 下载 / 单元格转义辅助
function downloadCSV(csv, filename) {
    const blob = new Blob([csv], { type: "text/csv;charset=utf-8;" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}
function csvCell(v) {
    const s = String(v == null ? "" : v);
    return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

function loadFaultHistory() {
    const list = $("faultHistoryList");
    if (!list) return;
    _initFaultRangeDefaults(); // 首次进入告警页时预填导出区间(默认近30天)
    if (!_faultHistoryLoaded) {
        list.innerHTML = '<div style="color:var(--text-muted);padding:20px;text-align:center">加载中...</div>';
    }
    fetch("/api/faults?limit=300", { cache: "no-store" })
        .then(r => r.json())
        .then(res => {
            if (!res || res.ok === false) {
                list.innerHTML = '<div style="color:var(--accent-red);padding:20px;text-align:center">加载失败: ' + htmlEsc((res && res.msg) || "未知错误") + '</div>';
                return;
            }
            // 仅取 fault 非零记录
            const rows = (res.data || []).filter(r => Number(r.fault) > 0);
            // 按故障码(单 bit)去重, 保留每个故障码最近一次上报(recv_time 最大)
            const byCode = {};
            rows.forEach(r => {
                const recv = Number(r.recv_time || 0);
                parseFault(r.fault).forEach(c => {
                    const prev = byCode[c.code];
                    if (!prev || recv > prev.recv) {
                        byCode[c.code] = { code: c.code, level: c.level, desc: c.desc, recv: recv };
                    }
                });
            });
            const items = Object.keys(byCode).map(k => byCode[k]).sort((a, b) => b.recv - a.recv);
            if (items.length === 0) {
                list.innerHTML = '<div style="color:var(--text-muted);padding:20px;text-align:center">✓ 暂无历史故障记录</div>';
                _faultHistoryItems = [];
                _faultHistoryLoaded = true;
                return;
            }
            let html = "";
            const shown = [];
            items.forEach(it => {
                if (_hiddenFaultCodes.has(it.code)) return;
                const cls = levelToClass(it.level);
                const icon = levelToIcon(it.level);
                const hex = "0x" + it.code.toString(16).toUpperCase().padStart(5, "0");
                const t = fmtDateTime(it.recv);
                html += '<div class="alert-item ' + cls + '" data-fault-code="' + it.code + '">' +
                        '<div class="alert-icon">' + icon + '</div>' +
                        '<div class="alert-body">' +
                        '<div class="alert-msg">' + htmlEsc(it.desc) + '</div>' +
                        '<div class="alert-meta"><span>故障码: ' + hex + '</span><span>🕐 ' + t + '</span><span style="color:var(--text-muted)">历史</span></div>' +
                        '</div>' +
                        '<div class="alert-actions"><button class="alert-btn" onclick="deleteHistoryFault(' + it.code + ')">删除</button></div>' +
                        '</div>';
                shown.push(it);
            });
            _faultHistoryItems = shown;
            if (shown.length === 0) {
                list.innerHTML = '<div style="color:var(--text-muted);padding:20px;text-align:center">✓ 已无历史故障记录（本会话已隐藏）</div>';
                _faultHistoryLoaded = true;
                return;
            }
            list.innerHTML = html;
            _faultHistoryLoaded = true;
        })
        .catch(err => {
            list.innerHTML = '<div style="color:var(--accent-red);padding:20px;text-align:center">加载失败: ' + htmlEsc(String(err)) + '</div>';
        });
}

// 由后端数据库按所选时间区间导出故障 CSV(审计级, 不受前端隐藏影响)
// 读取两个 datetime-local 输入 -> 转 epoch 秒 -> GET /api/faults/export -> blob 下载
let _faultRangeInited = false;
function _initFaultRangeDefaults() {
    if (_faultRangeInited) return;
    const sEl = $("faultStart"), eEl = $("faultEnd");
    if (!sEl || !eEl) return;
    const toLocal = d => {
        const off = d.getTimezoneOffset() * 60000;
        return new Date(d.getTime() - off).toISOString().slice(0, 16); // YYYY-MM-DDTHH:MM (本地)
    };
    const now = new Date();
    eEl.value = toLocal(now);
    sEl.value = toLocal(new Date(now.getTime() - 30 * 86400 * 1000));
    _faultRangeInited = true;
}

function exportFaultHistoryServer() {
    _initFaultRangeDefaults();
    const sEl = $("faultStart"), eEl = $("faultEnd");
    let start = null, end = null;
    if (sEl && sEl.value) {
        const t = new Date(sEl.value).getTime();
        if (!isNaN(t)) start = Math.floor(t / 1000);
    }
    if (eEl && eEl.value) {
        const t = new Date(eEl.value).getTime();
        if (!isNaN(t)) end = Math.floor(t / 1000);
    }
    if (end === null) end = Math.floor(Date.now() / 1000);
    if (start === null) start = end - 30 * 86400; // 默认近 30 天
    if (start > end) { showToast("起始时间不能晚于结束时间", "warn"); return; }
    const url = "/api/faults/export?start=" + start + "&end=" + end;
    showToast("正在从服务器导出故障记录...", "info");
    fetch(url, { cache: "no-store" })
        .then(r => {
            if (!r.ok) return r.json().then(j => { throw new Error((j && j.msg) || ("HTTP " + r.status)); });
            return r.blob();
        })
        .then(blob => {
            const a = document.createElement("a");
            a.href = URL.createObjectURL(blob);
            a.download = "BMS_历史故障_" +
                new Date(start * 1000).toISOString().slice(0, 10) + "_" +
                new Date(end * 1000).toISOString().slice(0, 10) + ".csv";
            document.body.appendChild(a); a.click(); a.remove();
            setTimeout(() => URL.revokeObjectURL(a.href), 1000);
            addOperationLog("导出历史故障 CSV(服务器)", "success");
            showToast("历史故障已导出(服务器 · 审计级)", "success");
        })
        .catch(err => { showToast("导出失败: " + err.message, "error"); });
}

// 前端隐藏某条历史故障(本会话内; 刷新后从 DB 重新加载会重现, 物理删除需后端接口)
function deleteHistoryFault(code) {
    code = Number(code);
    _hiddenFaultCodes.add(code);
    const list = $("faultHistoryList");
    if (list) {
        const el = list.querySelector('[data-fault-code="' + code + '"]');
        if (el) el.remove();
        _faultHistoryItems = _faultHistoryItems.filter(it => it.code !== code);
        if (list.children.length === 0) {
            list.innerHTML = '<div style="color:var(--text-muted);padding:20px;text-align:center">✓ 已无历史故障记录（本会话已隐藏）</div>';
        }
    }
    addOperationLog("隐藏历史故障: 0x" + code.toString(16).toUpperCase(), "info");
}

// 导出图表为 PNG 图片(Chart.js 渲染到 canvas, 直接 toDataURL)
function exportChartPNG(canvasId, name) {
    const cv = document.getElementById(canvasId);
    if (!cv) { showToast("图表未就绪", "warn"); return; }
    let url;
    try { url = cv.toDataURL("image/png"); }
    catch (e) { showToast("图表导出失败: " + e, "error"); return; }
    const a = document.createElement("a");
    a.href = url;
    a.download = name + "_" + new Date().toISOString().slice(0, 10) + ".png";
    document.body.appendChild(a); a.click(); a.remove();
    addOperationLog("导出图表图片: " + name, "success");
    showToast("图表已导出 PNG", "success");
}

// 清空全部实时告警(瞬态, 下次 socket 推送会重新出现)
function clearAllAlerts() {
    currentAlerts = [];
    const list = $("alertList");
    if (list) list.innerHTML = '<div style="color:var(--text-muted);padding:20px;text-align:center">✓ 暂无告警</div>';
    if ($("alertCount")) $("alertCount").textContent = 0;
    if ($("alertBadge")) $("alertBadge").textContent = 0;
    addOperationLog("已清空全部实时告警", "info");
}

// ============================================================
// 数据来源标识(真实/历史缓存/无数据)
// 注: 2026-08-06 起彻底移除模拟数据生成, is_mock 仅用于标识"无任何有效数据"状态
// ============================================================
function updateDeviceOnlineUI(online, devStatusStr) {
    /* 严格二值显示: 只分在线/离线, 无中间态 */
    const dot = $("connStatusDot");
    const txt = $("connStatusText");
    const pill = $("connStatusPill");
    if (dot && txt) {
        if (online) {
            dot.className = "status-dot online";
            txt.textContent = "设备在线";
            if (pill) {
                pill.style.color = "var(--text-primary)";
                pill.style.opacity = "1";
            }
        } else {
            dot.className = "status-dot";
            dot.style.background = "var(--accent-red)";
            const reason = (devStatusStr && devStatusStr !== "OFFLINE") ? " (" + devStatusStr + ")" : "";
            txt.textContent = "设备离线" + reason;
            if (pill) {
                pill.style.color = "var(--accent-red)";
                pill.style.opacity = "0.95";
            }
        }
    }
    // 页脚第一个: 同样严格二值
    const fDot = $("footerDevDot");
    const fItem = $("footerDeviceStatus");
    if (fDot && fItem) {
        if (online) {
            fDot.style.background = "var(--accent-green)";
            fItem.style.color = "";
            fItem.innerHTML = fDot.outerHTML + " 设备在线";
        } else {
            fDot.style.background = "var(--accent-red)";
            fItem.style.color = "var(--accent-red)";
            const reason = (devStatusStr && devStatusStr !== "OFFLINE") ? "(" + devStatusStr + ")" : "";
            fItem.innerHTML = fDot.outerHTML + " 设备离线" + reason;
        }
    }
}

function updateDataSource(isMock, detail, deviceStatus) {
    // 在 nav-status 区域显示数据来源标识
    // isMock=True 仅表示"无任何有效数据", 不代表生成了假数据
    // 正常情况: isMock=False, detail 中各字段标识 real(实时) 或 db_cache(历史缓存)
    let el = $("dataSourceBadge");
    let detailEl = $("dataSourceDetail");
    if (!el) {
        const navStatus = document.querySelector(".nav-status");
        if (navStatus) {
            const wrap = document.createElement("div");
            wrap.style.cssText = "display:flex;flex-direction:column;gap:2px;align-items:flex-end;line-height:1.1";
            el = document.createElement("span");
            el.id = "dataSourceBadge";
            el.className = "status-pill";
            el.style.cssText = "padding:4px 10px;border-radius:12px;font-size:11px;font-weight:600";
            detailEl = document.createElement("span");
            detailEl.id = "dataSourceDetail";
            detailEl.style.cssText = "font-size:10px;color:var(--text-muted)";
            wrap.appendChild(el);
            wrap.appendChild(detailEl);
            navStatus.insertBefore(wrap, navStatus.firstChild);
        }
    }
    if (!el) return;

    const REAL_DOT  = '<span class="status-dot online" style="width:6px;height:6px"></span>';
    const MIX_DOT   = '<span class="status-dot" style="background:var(--accent-yellow);width:6px;height:6px"></span>';
    const MOCK_DOT  = '<span class="status-dot" style="background:var(--accent-red);width:6px;height:6px"></span>';

    // 确定整体模式
    let mode = "real";              // real / mix / mock
    if (isMock) {
        if (detail) {
            const realCount = Object.values(detail).filter(v => v === "real").length;
            mode = realCount > 0 ? "mix" : "mock";
        } else {
            mode = "mock";
        }
    }

    if (mode === "real") {
        el.innerHTML = REAL_DOT + " 真实数据";
        el.style.background = "rgba(16,185,129,0.12)";
        el.style.color = "var(--accent-green)";
    } else if (mode === "mix") {
        el.innerHTML = MIX_DOT + " 混合模式";
        el.style.background = "rgba(245,158,11,0.15)";
        el.style.color = "var(--accent-yellow)";
    } else {
        el.innerHTML = MOCK_DOT + (deviceStatus === "设备离线" ? " 设备离线" : " 等待数据");
        el.style.background = "rgba(239,68,68,0.12)";
        el.style.color = "var(--accent-red)";
    }

    // 详细字段说明
    if (detailEl && detail) {
        const labelMap = { soc: "SOC", soh: "SOH", voltage: "电压", current: "电流", temp: "温度", fault: "故障" };
        const parts = [];
        for (const k of Object.keys(labelMap)) {
            if (detail[k]) parts.push(labelMap[k] + ":" + (detail[k] === "real" ? "实" : "模"));
        }
        let text = parts.join(" · ");
        if (deviceStatus) text = deviceStatus + "｜" + text;
        detailEl.textContent = text;
    } else if (detailEl) {
        detailEl.textContent = deviceStatus || "";
    }
}

// ============================================================
// 手动刷新
// ============================================================
function manualRefresh() {
    // 2026-08-19 修复(C5): refreshBtn 可能不存在(移动端/未渲染), 无空值保护会抛异常中断
    const btn = $("refreshBtn");
    if (!btn) return;
    btn.disabled = true;
    btn.textContent = "⏳ 刷新中...";
    fetch("/api/refresh", { method: "POST" })
        .then(r => r.json())
        .then(res => {
            addLog(res.ok ? "[刷新] 已触发立即刷新" : "[刷新] 触发失败: " + (res.msg || ""),
                   res.ok ? "log-success" : "log-error");
            // 修复: 触发后立即拉取一次状态, 不等 SocketIO 推送(约3秒)
            if (res.ok) {
                setTimeout(() => updateCommStatus(), 500);
                setTimeout(() => updateCommStatus(), 2000);
            }
        })
        .catch(err => addLog("[刷新] 异常: " + err, "log-error"))
        .finally(() => {
            setTimeout(() => {
                const b2 = $("refreshBtn");
                if (b2) { b2.disabled = false; b2.textContent = "🔄 刷新"; }
            }, 1500);
        });
}

// ============================================================
// 一键诊断: 调用后端 /api/diag, 结果渲染到弹窗面板(日常维护排查用)
// 检查项: 设备状态 / 上报新鲜度 / 字段新旧 / packV 值 / 综合问题
// ============================================================
function openDiagModal() {
    const m = $("diagModal");
    if (m) m.classList.add("show");
    /* 2026-09-15 系统化排查: 导出按钮常显(诊断前禁用), 避免用户找不到导出入口 */
    const eb = $("diagExportBtn");
    if (eb) {
        eb.style.display = "";
        eb.disabled = !window._lastDiagData;
        eb.textContent = window._lastDiagData ? "📥 导出 TXT" : "📥 导出 TXT(先诊断)";
    }
}
function closeDiagModal() {
    const m = $("diagModal");
    if (m) m.classList.remove("show");
}

// 诊断结果一行: <k, v, cssClass>
function diagRow(k, v, cls) {
    return '<div class="diag-row"><span class="diag-k">' + k + "</span>"
         + '<span class="diag-v' + (cls ? " " + cls : "") + '">' + v + "</span></div>";
}

/* ====== 2026-09-15: 一键诊断结果导出 TXT(日志样式留档) ======
 * 前端 Blob 生成, 零后端改动; 内容 = 日志风格纯文本(键值对+分区标题+原始JSON附录),
 * 文件名 BMS_diag_YYYYmmdd_HHMMSS.txt, 供现场排查留档/微信转发。 */
function exportDiagTxt() {
    const d = window._lastDiagData;
    if (!d) { showToast("请先运行一次诊断", "warn"); return; }
    const L = [];
    const line = "= ".repeat(32);
    L.push(line);
    L.push("  BMS 一键诊断报告");
    L.push("  生成时间: " + (d.time_str || new Date().toLocaleString()));
    L.push(line);
    L.push("");
    L.push("【1. 连接状态】");
    L.push("  诊断时间       : " + (d.time_str || "-"));
    L.push("  后端→华为云    : " + (d.cloud_accessible ? "✅ 可达" : "❌ 不可达"));
    L.push("  设备状态       : " + (d.device_online ? "✅ 在线" : "❌ 离线") + " (华为云=" + (d.device_last_status || "?") + ")");
    L.push("  数据库总条数   : " + (d.db_count != null ? d.db_count : "-"));
    L.push("");
    L.push("【2. 上报新鲜度】");
    if (d.shadow_last_report_age_hint != null) {
        L.push("  最近上报       : " + d.shadow_last_report_age_hint
             + (d.shadow_last_report_age_sec >= 300 ? " ⚠️ 超过5分钟, 影子陈旧!" : ""));
    } else {
        L.push("  最近上报       : 未知(影子无有效 event_time)");
    }
    if (d.shadow_last_report_time) L.push("  上报时刻       : " + d.shadow_last_report_time);
    L.push("");
    L.push("【3. 字段版本判定】");
    const fv = d.field_version || "?";
    L.push("  版本判定       : " + (fv === "new" ? "新固件+新物模型" : fv === "old" ? "旧固件(需烧录新固件)" : fv)
         + (d.shadow_prop_count ? " (" + d.shadow_prop_count + " 个属性)" : ""));
    if (d.fields_new && d.fields_new.length) L.push("  新字段         : " + d.fields_new.join(", "));
    if (d.fields_old && d.fields_old.length) L.push("  旧字段         : " + d.fields_old.join(", "));
    L.push("");
    L.push("【4. 关键数值】");
    L.push("  总压           : " + (d.pack_v != null ? (d.pack_v / 1000).toFixed(2) + " V" : "?"));
    L.push("  SOC            : " + (d.soc != null ? Math.round(Number(d.soc)) + "%" : "?"));
    L.push("  电流           : " + (d.current != null ? (d.current / 1000).toFixed(2) + " A" : "?"));
    if (d.cell_voltages_all_zero) L.push("  单体电压       : ⚠️ 全为 0 (检查 BQ76952 接线 / HW_ENABLE_BQ76952 配置)");
    L.push("");
    L.push("【5. 数据库写入】");
    L.push("  最近写入       : " + (d.db_last_write_ago_sec != null ? d.db_last_write_ago_sec + " 秒前 (共 " + (d.db_count || 0) + " 条)" : "?"));
    L.push("");
    L.push(line);
    L.push("附录: 诊断原始 JSON");
    L.push(line);
    L.push(JSON.stringify(d, null, 2));

    const blob = new Blob(["\ufeff" + L.join("\r\n")], { type: "text/plain;charset=utf-8" });
    const t = new Date();
    const p2 = n => String(n).padStart(2, "0");
    const fname = "BMS_diag_" + t.getFullYear() + p2(t.getMonth() + 1) + p2(t.getDate())
                + "_" + p2(t.getHours()) + p2(t.getMinutes()) + p2(t.getSeconds()) + ".txt";
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = fname;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 800);
    addLog("[诊断] 已导出 " + fname, "log-success");
    showToast("诊断报告已下载: " + fname, "success");
}

function runDiag() {
    // 2026-09-16 残留修复: 手机端 mobDiagBtn 无 disabled 态, 连点会叠加请求 → 防重入.
    // 2026-09-17 修复: 原守卫检查 #diagLoading 节点, 但该节点在请求失败/DOM残留时会
    // 永久存在 → 守卫永久拦截, 表现为"点击无反应"。改为检查弹窗 .show 态(关窗即复位)。
    const _dm = document.getElementById("diagModal");
    if (_dm && _dm.classList.contains("show")) return;
    const btn = $("diagBtn");
    if (btn) { btn.disabled = true; btn.textContent = "⏳ 诊断中..."; }
    openDiagModal();
    const body = $("diagBody");
    const badge = $("diagHealthBadge");
    if (body) body.innerHTML = '<div class="diag-loading" id="diagLoading">⏳ 诊断中...</div>';
    if (badge) badge.innerHTML = "";
    const exportBtn = $("diagExportBtn");
    /* 2026-09-15 系统化排查: 按钮常显, 新诊断开始时置禁用(不再隐藏) */
    if (exportBtn) { exportBtn.disabled = true; exportBtn.textContent = "⏳ 诊断中..."; }
    fetch("/api/diag", { cache: "no-store" })
        .then(r => r.json())
        .then(d => {
            if (!d || !d.ok) {
                // 2026-08-19 修复(C1): JSON.stringify 结果经 htmlEsc 转义, 防反射 XSS/破 DOM
                if (body) body.innerHTML = '<div class="diag-section"><div class="diag-row"><span class="diag-err">接口异常: '
                    + htmlEsc(JSON.stringify(d)) + "</span></div></div>";
                return;
            }
            // 标题健康徽章
            if (badge) {
                badge.textContent = d.health === "OK" ? "✓ 健康" : "⚠ 异常";
                badge.className = d.health === "OK" ? "diag-health-ok" : "diag-health-warn";
            }
            let html = "";
            // 1. 连接状态
            html += '<div class="diag-section"><div class="diag-section-title">连接状态</div>';
            html += diagRow("诊断时间", d.time_str || "-", "diag-info");
            window._lastDiagData = d;   // 2026-09-15: 保存本次诊断结果供"导出 TXT"使用
            html += diagRow("后端→华为云", d.cloud_accessible ? "✅ 可达" : "❌ 不可达", d.cloud_accessible ? "diag-ok" : "diag-err");
            html += diagRow("设备状态", (d.device_online ? "✅ 在线" : "❌ 离线") + " (华为云=" + (d.device_last_status || "?") + ")",
                            d.device_online ? "diag-ok" : "diag-err");
            html += "</div>";
            // 2. 上报新鲜度
            html += '<div class="diag-section"><div class="diag-section-title">上报新鲜度</div>';
            if (d.shadow_last_report_age_hint) {
                const stale = d.shadow_last_report_age_sec != null && d.shadow_last_report_age_sec > 300;
                html += diagRow("最近上报", d.shadow_last_report_age_hint + (stale ? " ⚠️ 超过5分钟, 影子陈旧!" : ""),
                                stale ? "diag-warn" : "diag-ok");
            } else {
                html += diagRow("最近上报", "未知(影子无有效 event_time)", "diag-warn");
            }
            html += "</div>";
            // 3. 字段新旧
            const fv = d.field_version || "unknown";
            const fvTxt = fv === "new" ? "✅ 新物模型字段(正确)" :
                          fv === "old" ? "❌ 旧字段名(需烧录新固件)" :
                          fv === "mixed" ? "⚠️ 新旧字段混合" : "❓ 无法判定";
            const fvCls = fv === "new" ? "diag-ok" : (fv === "old" ? "diag-err" : "diag-warn");
            html += '<div class="diag-section"><div class="diag-section-title">字段版本</div>';
            html += diagRow("版本判定", fvTxt + (d.shadow_prop_count ? " (" + d.shadow_prop_count + " 个属性)" : ""), fvCls);
            if (d.fields_new && d.fields_new.length) html += diagRow("新字段", d.fields_new.join(", "), "diag-info");
            if (d.fields_old && d.fields_old.length) html += diagRow("旧字段", d.fields_old.join(", "), "diag-warn");
            html += "</div>";
            // 4. 关键值
            html += '<div class="diag-section"><div class="diag-section-title">关键数值</div>';
            html += diagRow("总压", d.pack_v != null ? (d.pack_v / 1000).toFixed(2) + " V" : "?");
            html += diagRow("SOC", d.soc != null ? Math.round(Number(d.soc)) + "%" : "?");
            html += diagRow("电流", d.current != null ? (d.current / 1000).toFixed(2) + " A" : "?");
            if (d.cell_voltages_all_zero) html += diagRow("单体电压", "⚠️ 全为 0 (检查 BQ76952 接线 / HW_ENABLE_BQ76952 配置)", "diag-warn");
            html += "</div>";
            // 5. 数据库写入
            if (d.db_last_write_ago_sec != null) {
                html += '<div class="diag-section"><div class="diag-section-title">数据库</div>';
                html += diagRow("最近写入", d.db_last_write_ago_sec + " 秒前 (共 " + (d.db_count || 0) + " 条)", "diag-info");
                html += "</div>";
            }
            // 6. 综合问题
            html += '<div class="diag-section"><div class="diag-section-title">综合问题</div>';
            if (d.problems && d.problems.length) {
                d.problems.forEach(p => { html += '<div class="diag-problem">' + p + "</div>"; });
            } else {
                html += '<div class="diag-none">✅ 未发现明显问题</div>';
            }
            html += "</div>";
            // 2026-09-15 系统化排查: 主区块的异常项自动附中文排查指引
            const _hints = [];
            if (fv === "old") _hints.push("<b>旧固件</b>: 设备仍在用旧物模型字段(v_max/cells等) → 烧录新固件并确认物模型已更新");
            if (d.shadow_last_report_age_sec >= 300) _hints.push("<b>上报陈旧</b>: 已 " + Math.round(d.shadow_last_report_age_sec / 60) + " 分钟无上报 → 查设备电源/网络/MQTT 连接");
            if (d.cell_voltages_all_zero) _hints.push("<b>单体电压全 0</b>: AFE 未采到电芯 → 查 bq76952 接线/I2C 地址 0x10/HW_ENABLE_BQ76952 配置");
            if (d.db_last_write_ago_sec != null && d.db_last_write_ago_sec >= 300) _hints.push("<b>数据库停写</b>: " + Math.round(d.db_last_write_ago_sec / 60) + " 分钟无新记录 → 后端消费腿掉线, 查 bms-dashboard 服务与 EMQX 订阅");
            if (_hints.length) html += '<div class="diag-hint">🔧 排查指引: ' + _hints.join("<br>🔧 ") + "</div>";
            // 7. 华为云影子详情(2026-08-21 #5): 追加 /api/debug/shadow 数据
            html += '<div class="diag-section"><div class="diag-section-title">华为云影子详情 <span style="font-size:11px;color:var(--text-muted)">(debug/shadow)</span></div>';
            html += '<div class="diag-row" id="shadowDiagRow"><span class="diag-k">影子数据</span><span class="diag-v diag-info">加载中...</span></div>';
            html += "</div>";
            if (body) body.innerHTML = html;
            // 2026-09-15: 诊断成功 → 导出按钮恢复可用(常显设计)
            const _eb = $("diagExportBtn");
            if (_eb) { _eb.disabled = false; _eb.textContent = "📥 导出 TXT"; }
            // 影子详情异步加载
            fetch("/api/debug/shadow", { cache: "no-store" })
                .then(r => r.json())
                .then(sh => {
                    const row = $("shadowDiagRow");
                    if (!row) return;
                    if (!sh || !sh.ok) {
                        row.innerHTML = '<span class="diag-k">影子数据</span><span class="diag-v diag-err">接口异常: ' + htmlEsc(JSON.stringify(sh)) + "</span>";
                        return;
                    }
                    const ls = sh.live_status || {};
                    const last = sh.last_shadow || {};
                    const st = ls.status || "?";
                    /* 2026-09-15 系统化排查: 英文状态→中文翻译; 每行附排查指引(diag-hint) */
                    const stCn = st === "ONLINE" ? "✅ 在线(ONLINE)" : st === "OFFLINE" ? "❌ 离线(OFFLINE)" : "❓ 未知(" + st + ")";
                    const ageSec = sh.shadow_age_ms != null ? Math.round(sh.shadow_age_ms / 1000) : null;
                    const rows = [
                        ["设备状态", sh.device_online ? "✅ 在线" : "❌ 离线", sh.device_online ? "diag-ok" : "diag-err"],
                        ["华为云连接", stCn, st === "ONLINE" ? "diag-ok" : "diag-warn"],
                        ["影子是否有数据", sh.shadow_no_data ? "⚠️ 无数据(no_data)" : "✅ 有数据", sh.shadow_no_data ? "diag-warn" : "diag-ok"],
                        ["影子属性数", (last.props && Object.keys(last.props).length) != null ? Object.keys(last.props).length + " 个" : "?"],
                        ["影子数据年龄", ageSec != null ? (ageSec < 60 ? ageSec + " 秒前" : ageSec < 3600 ? Math.round(ageSec / 60) + " 分钟前" : Math.round(ageSec / 3600) + " 小时前") : "?"],
                    ];
                    const hints = [];
                    if (st !== "ONLINE") hints.push("<b>华为云连接异常</b>: 设备与华为云 MQTT 断开 → 查设备 WiFi/4G 是否在线、固件 MQTT 日志有无断连重连");
                    if (sh.shadow_no_data) hints.push("<b>影子无数据</b>: 设备从未上报或已被清空 → 查设备上报日志 / 确认固件 MQTT 主题与物模型匹配");
                    if (ageSec != null && ageSec >= 300) hints.push("<b>影子数据陈旧(" + Math.round(ageSec / 60) + "分钟前)</b>: 设备已停止上报 → 查设备是否掉电/断网, 或上报间隔配置");
                    row.innerHTML = rows.map(r2 => '<div class="diag-row"><span class="diag-k">' + r2[0] + "</span><span class='diag-v " + (r2[2] || "") + "'>" + r2[1] + "</span></div>").join("")
                        + (hints.length ? '<div class="diag-hint">🔧 排查指引: ' + hints.join("<br>🔧 ") + "</div>" : "");
                    // 展开原始影子 JSON
                    if (sh.last_shadow) {
                        const raw = document.createElement("pre");
                        raw.style.cssText = "margin:8px 0 0;padding:8px;background:var(--bg-input);border-radius:6px;font-size:11px;max-height:220px;overflow:auto;white-space:pre-wrap;word-break:break-all";
                        raw.textContent = JSON.stringify(sh.last_shadow, null, 2);
                        row.appendChild(raw);
                    }
                })
                .catch(err => {
                    const row = $("shadowDiagRow");
                    if (row) row.innerHTML = '<span class="diag-k">影子数据</span><span class="diag-v diag-err">调用失败: ' + htmlEsc(String(err)) + "</span>";
                });
        })
        .catch(err => {
            // 2026-08-19 修复(C1): err 经 htmlEsc 转义, 防反射 XSS/破 DOM
            if (body) body.innerHTML = '<div class="diag-section"><div class="diag-row"><span class="diag-err">调用失败: '
                + htmlEsc(String(err)) + "</span></div></div>";
        })
        .finally(() => {
            setTimeout(() => {
                if (btn) { btn.disabled = false; btn.textContent = "🔍 诊断"; }
            }, 1500);
        });
}

// 倒计时显示状态
let nextQuerySeconds = 0;
let lastCountdownTime = 0;

function updateCountdown() {
    if (nextQuerySeconds > 0) {
        const now = Date.now();
        if (lastCountdownTime > 0) {
            const delta = Math.round((now - lastCountdownTime) / 1000);
            nextQuerySeconds = Math.max(0, nextQuerySeconds - delta);
        }
        lastCountdownTime = now;
        const info = $("nextQueryInfo");
        if (info) {
            info.textContent = "下次查询: " + nextQuerySeconds + "s";
        }
        if (nextQuerySeconds === 0 && info) {
            info.textContent = "";
        }
    }
}

// ============================================================
// 通信状态更新(从 /api/status)
// ============================================================
// 2026-08-09 多设备: 当前选中设备 ID(空=主设备), 切换时由 onDeviceSwitch 设置
let _currentDeviceId = "";
// 2026-08-11: 缓存 bms_info 中的固件版本, 供侧边栏实时展示
let _cachedFirmware = "--";
// 2026-08-11: 缓存 /api/devices 设备列表, 供侧边栏"设备"字段实时回显当前选中设备
let _deviceList = [];
// 2026-08-20 修复(#2 防闪): 轮询路径离线状态去抖计数(连续 2 次轮询确认才转离线 UI)
let _devOnlineDebounce = 0;

function updateCommStatus() {
    const _q = _currentDeviceId ? ("?device=" + encodeURIComponent(_currentDeviceId)) : "";
    // 2026-08-10: 与 /api/history 一致, 加 15s 超时(AbortController)——后端卡死时
    //   避免 fetch 无限挂起导致 15s 定时轮询堆积、通信状态卡片长期不刷新
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(), 15000);
    fetch("/api/status" + _q, { signal: ac.signal })
        .then(r => r.json())
        .then(s => {
            clearTimeout(timer);
            // 更新倒计时基准
            if (s.next_query_in && s.next_query_in > 0) {
                nextQuerySeconds = s.next_query_in;
                lastCountdownTime = Date.now();
                const info = $("nextQueryInfo");
                if (info) info.textContent = "下次查询: " + s.next_query_in + "s";
            }

            // ===== 严格二值在线判定: 在线 / 离线, 不搞中间态 =====
            // 在线 = 设备 ON 状态 且 数据年龄 <=200s (避免断网后API缓存未刷新误判)
            const OFFLINE_MS = 10 * 1000;   // 2026-08-21: 20s→10s(用户要求 10s 内判定离线)
            let ageMs = Number(s.data_age_ms || 0);
            if (!ageMs && typeof s.last_event_time_ms === "number" && s.last_event_time_ms > 946684800 * 1000) {
                ageMs = Math.max(0, Date.now() - s.last_event_time_ms);
            }
            // 2026-08-18 离线判定增强: EMQX 直连新鲜度(emqx_age_ms)优先。
            //   设备断电瞬间 EMQX 消费腿即不再收到帧, emqx_age_ms 迅速增大,
            //   用它判定离线比华为云影子(60~120s)快得多 —— 断电后 ~60s 内前端转离线。
            if (typeof s.emqx_age_ms === "number" && s.emqx_age_ms >= 0) {
                ageMs = Math.max(ageMs, s.emqx_age_ms);
            }
            let apiOnline = (typeof s.device_online === "boolean") ? !!s.device_online : (s.device_status === "ONLINE");
            if ((s.data_realtime === true) && !apiOnline) apiOnline = true;
            // 2026-08-18: EMQX 直连已确认无新帧(>60s) 且 华为云也判离线 → 强制离线;
            //   若仅华为云慢半拍(EMQX 还有新帧), 仍以 EMQX 新鲜度为准判在线
            const ageOffline = (ageMs > 0) && (ageMs > OFFLINE_MS);
            const emqxAlive = (typeof s.emqx_age_ms === "number" && s.emqx_age_ms >= 0 && s.emqx_age_ms <= OFFLINE_MS);
            // 2026-08-20 修复(#2 防闪): EMQX 直连新鲜 = 设备在线, 权威优先。
            //   华为云设备详情 API 有 1~2min 延迟, 其影子 age 可能误报陈旧,
            //   若仍用 `&& !(ageMs > OFFLINE_MS)` 会把在线设备判离线(一秒在线一秒离线闪)。
            const devOnline = emqxAlive || ((apiOnline && !ageOffline) && !(ageMs > OFFLINE_MS));
            // 2026-08-22 改进(C1): 离线/故障告警(声音+桌面通知, 内部 60s 节流)
            try { alertOnStateChange(devOnline, Number(s.fault || 0)); } catch (e) {}

            const ageTxt = (() => {
                if (!ageMs) return "";
                if (ageMs < 60000) return (Math.round(ageMs/1000) + "s");
                if (ageMs < 3600000) return (Math.round(ageMs/60000) + "min");
                return (ageMs/3600000).toFixed(1) + "h";
            })();

            // ===== 设备自报通信状态位域(comm_status) =====
            // 优先用设备真实上报的位域驱动四张卡片; 旧固件/未更新云物模型时回退推断.
            const CS_WIFI = 1<<0, CS_MQTT = 1<<1, CS_TLS = 1<<2, CS_CLOUD = 1<<3, CS_CAN_OK = 1<<4, CS_CAN_DIS = 1<<5;
            const cs = (typeof s.comm_status === "number") ? s.comm_status : 0;
            const csValid = (typeof s.comm_status === "number");
            const csWifi   = csValid && (cs & CS_WIFI);
            const csMqtt   = csValid && (cs & CS_MQTT);
            const csTls    = csValid && (cs & CS_TLS);
            const csCloud  = csValid && (cs & CS_CLOUD);
            const csCanOk  = csValid && (cs & CS_CAN_OK);
            const csCanDis = csValid && (cs & CS_CAN_DIS);
            const srcTag = csValid ? " (设备自报)" : " (推断)";

            // ===== MQTT KPI 卡片: 严格二值 =====
            const mqttVal = $("mqttVal");
            const mqttDetail = $("mqttDetail");
            if (mqttVal) {
                if (csValid) {
                    if (csMqtt && csCloud) {
                        mqttVal.textContent = "已连接";
                        mqttVal.style.color = "var(--accent-green)";
                    } else if (csWifi) {
                        mqttVal.textContent = "WiFi已连·MQTT未连";
                        mqttVal.style.color = "var(--accent-yellow)";
                    } else if (csMqtt) {
                        mqttVal.textContent = "已连·云未确认";
                        mqttVal.style.color = "var(--accent-yellow)";
                    } else {
                        mqttVal.textContent = "未连接";
                        mqttVal.style.color = "var(--accent-red)";
                    }
                } else {
                    // 回退: 原推断逻辑(兼容旧固件)
                    const cloudDown = (s.cloud_accessible === false || s.iotda_connected === false);
                    if (cloudDown) {
                        mqttVal.textContent = "连接中...";
                        mqttVal.style.color = "var(--accent-yellow)";
                    } else if (devOnline) {
                        mqttVal.textContent = "已连接";
                        mqttVal.style.color = "var(--accent-green)";
                    } else {
                        mqttVal.textContent = "未连接";
                        mqttVal.style.color = "var(--accent-red)";
                    }
                }
            }
            const row1 = $("mqttRow1");
            const row2 = $("mqttRow2");
            // 2026-08-18 双链路融合: EMQX 直连(主, 100ms fast 帧) + 华为云(兜底, 7s/10s)
            //   emqx_direct_online = 后端->EMQX 消费腿在线(订阅 6 主题, 收到即刷新)
            //   emqx_age_ms        = 距最近一帧(telemetry/fast/data)毫秒数
            const emqxDirect = (typeof s.emqx_direct_online === "boolean") ? s.emqx_direct_online : false;
            const emqxAge = (typeof s.emqx_age_ms === "number") ? s.emqx_age_ms : -1;
            const emqxText = emqxDirect
                ? "EMQX直连: 在线" + (emqxAge >= 0 ? ("(" + (emqxAge < 60000 ? (Math.round(emqxAge/1000) + "s") : (Math.round(emqxAge/60000) + "min")) + "前)") : "")
                : "EMQX直连: 离线";
            const cloudText = (s.cloud_accessible || s.iotda_connected) ? "华为云: 正常" : "华为云: 异常";
            const devText = devOnline
                ? "设备: ONLINE" + (ageTxt ? (" (" + ageTxt + ")") : "")
                : "设备: OFFLINE" + (ageTxt ? (" (≥" + ageTxt + "无新数据)") : "");
            // 2026-08-11: 实时性预算展示(设备消息日上限15000 / 云端API用量)
            const _dmEst = Number(s.device_msg_day_estimate || 0);
            const _dmBud = Number(s.device_msg_budget || 15000);
            const _apiUsed = Number(s.cloud_api_used_24h || 0);
            const _apiBud = Number(s.cloud_api_budget || 15000);
            const _dmPct = _dmBud > 0 ? Math.round(_dmEst / _dmBud * 100) : 0;
            const _r1 = emqxText + " | " + cloudText + " | " + devText;
            const _r2 = "记录: " + (s.db_count || 0) +
                " | 消息: " + _dmEst + "/" + _dmBud + "/天(" + _dmPct + "%)" +
                " | API: " + _apiUsed + "/" + _apiBud;
            if (row1 && row2) {
                row1.textContent = _r1;
                row2.textContent = _r2;
            } else if (mqttDetail) {
                // 兼容旧版单排结构
                mqttDetail.textContent = _r1 + " | " + _r2;
            }
            // ===== TLS 加密状态卡片 (双链路: EMQX wss/TLS 主 + 华为云 mqtts 兜底) =====
            const tlsVal = $("tlsVal");
            const tlsDetail = $("tlsDetail");
            if (tlsVal && tlsDetail) {
                // 2026-08-18 双链路融合: 只要 EMQX 直连在线即有 wss(TLS) 通道加密;
                //   csTls 位 = 设备自报 MQTTS 安全通道已建立(华为云腿), 两者任一即"加密已建立"
                const emqxTlsAlive = emqxDirect && emqxAge >= 0 && emqxAge <= OFFLINE_MS;
                const hwTls = csValid ? csTls : (s.cloud_accessible || s.iotda_connected);
                const tlsOk = emqxTlsAlive || !!hwTls;
                if (tlsOk) {
                    tlsVal.textContent = "✓ 加密";
                    tlsVal.style.color = "var(--accent-green)";
                    // 双链路明细: EMQX wss 主 + 华为云 mqtts 兜底
                    const _emqxTxt = emqxTlsAlive ? "EMQX wss/TLS" : "EMQX 未连";
                    const _hwTxt = hwTls ? "华为云 mqtts" : "华为云 未连";
                    tlsDetail.textContent = _emqxTxt + " | " + _hwTxt + (csValid ? " (设备自报)" : "");
                    tlsDetail.className = "kpi-change up";
                } else {
                    tlsVal.textContent = "未建立";
                    tlsVal.style.color = "var(--accent-red)";
                    tlsDetail.textContent = "双链路均未建立加密通道";
                    tlsDetail.className = "kpi-change down";
                }
            }
            // ===== 链路融合状态卡片 (2026-08-18 由"设备影子"改造) =====
            //   双链路融合: EMQX 直连(主, 100ms fast 帧) + 华为云影子(兜底, 7s/10s)。
            //   值 = 融合后的数据新鲜度状态: 实时(EMQX 有帧) / 兜底(仅华为云) / 过期(双路均无)
            const shadowVal = $("shadowVal");
            const shadowDetail = $("shadowDetail");
            if (shadowVal && shadowDetail) {
                const emqxFresh = emqxDirect && emqxAge >= 0 && emqxAge <= OFFLINE_MS;
                const hwAlive = (s.cloud_accessible || s.iotda_connected) && devOnline;
                if (emqxFresh) {
                    // EMQX 主链路实时帧在流 → 融合状态"实时"(最高新鲜度)
                    shadowVal.textContent = "实时";
                    shadowVal.style.color = "var(--accent-green)";
                    shadowDetail.textContent = "EMQX 直连 " + (emqxAge >= 0 ? (Math.round(emqxAge/1000) + "s 前") : "") +
                        " | 华为云 " + (hwAlive ? "影子正常" : "影子待同步") + (csValid ? " (设备自报)" : "");
                    shadowDetail.className = "kpi-change up";
                } else if (hwAlive) {
                    // EMQX 断帧但华为云影子尚在线 → 兜底模式(数据延迟大)
                    shadowVal.textContent = "兜底";
                    shadowVal.style.color = "var(--accent-yellow)";
                    shadowDetail.textContent = "EMQX 直连断帧 | 华为云影子 " + (ageTxt || "--") + "前" + (csValid ? " (设备自报)" : "");
                    shadowDetail.className = "kpi-change";
                } else if (devOnline) {
                    shadowVal.textContent = "同步中";
                    shadowVal.style.color = "var(--accent-yellow)";
                    shadowDetail.textContent = "数据年龄: " + (ageTxt || "实时") + " | 记录: " + (s.db_count || 0) + (csValid ? " (设备自报)" : "");
                    shadowDetail.className = "kpi-change";
                } else {
                    shadowVal.textContent = "过期";
                    shadowVal.style.color = "var(--accent-red)";
                    shadowDetail.textContent = "双链路均无新数据 | 最后更新: " + (ageTxt || "--") + "前";
                    shadowDetail.className = "kpi-change down";
                }
            }
            // ===== CAN 总线状态卡片 (设备自报: csCanOk / csCanDis 位域) =====
            const canVal = $("canVal");
            const canDetail = $("canDetail");
            if (canVal && canDetail) {
                if (csValid) {
                    if (csCanDis) {
                        canVal.textContent = "未启用";
                        canVal.style.color = "var(--text-muted)";
                        canDetail.textContent = "CAN 硬件未配置(无屏蔽)" + srcTag;
                        canDetail.className = "kpi-change";
                    } else if (csCanOk) {
                        canVal.textContent = "正常";
                        canVal.style.color = "var(--accent-green)";
                        canDetail.textContent = "CAN 收发正常" + srcTag;
                        canDetail.className = "kpi-change up";
                    } else {
                        canVal.textContent = "未确认";
                        canVal.style.color = "var(--accent-yellow)";
                        canDetail.textContent = "CAN 已启用·暂未收到收发确认" + srcTag;
                        canDetail.className = "kpi-change";
                    }
                } else {
                    // 回退: 原推断逻辑(兼容旧固件)
                    if (devOnline) {
                        canVal.textContent = "正常";
                        canVal.style.color = "var(--accent-green)";
                        canDetail.textContent = "数据链路: 正常 | 上报间隔: 60s (推断)";
                        canDetail.className = "kpi-change up";
                    } else {
                        canVal.textContent = "无数据";
                        canVal.style.color = "var(--accent-red)";
                        canDetail.textContent = "设备离线 | CAN 链路不可用 (推断)";
                        canDetail.className = "kpi-change down";
                    }
                }
            }
            // footer / nav 中的延迟显示
            if ($("latency")) {
                if (!ageMs) {
                    $("latency").textContent = latencyMs > 0 ? latencyMs : "--";
                } else {
                    $("latency").textContent = ageTxt;
                }
            }
            // 页脚 MQTT: 同样严格二值
            const fMqtt = $("footerMqtt");
            if (fMqtt) {
                const cloudOk = (s.cloud_accessible || s.iotda_connected);
                if (!cloudOk) {
                    fMqtt.textContent = "📡 云端: 未连接 (访问华为云失败)";
                    fMqtt.style.color = "var(--accent-red)";
                } else if (devOnline) {
                    fMqtt.textContent = "📡 云端: 已连接 (" + (s.region || "--") + ") · 设备: 在线 (" + (ageTxt || "实时") + ")";
                    fMqtt.style.color = "var(--text-primary)";
                } else {
                    fMqtt.textContent = "📡 云端: 已连接 (" + (s.region || "--") + ") · 设备: 离线 (" + (ageTxt || (s.device_status || "--")) + ")";
                    fMqtt.style.color = "var(--accent-red)";
                }
            }
            // 数据库记录数(footer)
            const fDb = $("footerDbCount");
            if (fDb) fDb.textContent = "📊 记录: " + (s.db_count || 0);

            // 顶部导航/页脚设备状态: 严格二值(绿点/红点)
            if (typeof s.device_online !== "undefined" || s.device_status) {
                // 2026-08-20 修复(#2 防闪): 轮询路径去抖——离线判定需连续 2 次轮询
                //   (≥2×15s)确认才真正转离线 UI。设备断电的第一时间离线由 bms_status
                //   LWT 遗嘱事件负责(秒级、不去抖), 此处去抖只防华为云影子/桥接误报
                //   造成的"一秒在线一秒离线"循环闪。恢复在线则立即恢复, 不去抖。
                if (devOnline) {
                    _devOnlineDebounce = 0;
                    updateDeviceOnlineUI(true, s.device_status || "ONLINE");
                } else {
                    _devOnlineDebounce++;
                    if (_devOnlineDebounce >= 2) {
                        updateDeviceOnlineUI(false, s.device_status || "OFFLINE");
                        updateDataSource(true, null, "设备离线");
                    }
                }
            }

            addLog("[状态] 云端: " + ((s.cloud_accessible || s.iotda_connected) ? "已连接" : "未连接")
                + " · 设备: " + (devOnline ? ("在线(" + (ageTxt || "实时") + ")") : ("离线(" + (ageTxt || (s.device_status || "--")) + ")"))
                + " · 记录: " + (s.db_count || 0),
                (s.cloud_accessible || s.iotda_connected) ? (devOnline ? "log-info" : "log-warn") : "log-error");

            // ---------- 关键: /api/status 轮询也刷新 KPI/单体表格/曲线
            //   (应对 Cloudflare/SocketIO 未透传 WebSocket 的情况)
            //   no_data=true 时也需要调用 handleBmsData 以更新前端为"无数据"状态
            const hasData = s.no_data || s.data || typeof s.pack_v !== "undefined"
                || typeof s.current !== "undefined" || typeof s.soc !== "undefined";
            // 2026-08-20 修复(#2 防闪): EMQX 直连新鲜时, 华为云误报的 no_data
            //   不传给渲染层 —— 否则 KPI 一秒显示"--"、一秒恢复, 与闪烁同源。
            //   (华为云设备详情 API 1~2min 延迟, 设备在线时其影子可能仍标 no_data)
            const sForBms = (emqxAlive && s.no_data)
                ? Object.assign({}, s, { no_data: false }) : s;
            if (hasData) {
                try { handleBmsData(sForBms); } catch (e) { console.warn("handleBmsData fail:", e); }
            }
        })
        .catch(err => {
            // 2026-08-19 修复(C3): 原实现空 catch 静默吞错, 状态轮询失败运维无感知.
            //   加日志便于排查"通信状态卡片长期不更新"
            addLog("[状态] 轮询失败: " + (err && err.message ? err.message : err), "log-error");
        });
}

// ============================================================
// 控制开关(充放电总开关/FET、充放电 MOS、均衡)
// 改造: 真实下发 set_charge/set_discharge/set_balance/set_relay 到 ESP32
// ============================================================
function sendDeviceCmd(cmd, paras) {
    // 2026-08-10 D1 修复: 参数统一放 paras 子对象(与 iotda_client.publish_cmd
    //   只透传 paras/args 对齐)。原顶层平铺会导致 enable/charge/mask 等参数在
    //   后端被丢弃, 控制命令只能关不能开。后端 /api/cmd 已同时兼容顶层平铺兜底, 双保险。
    const body = { cmd: cmd, paras: paras || {} };
    return fetch("/api/cmd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
    }).then(r => {
        if (r.status === 401) {
            window.location.href = "/login";
            return { ok: false, msg: "未登录" };
        }
        if (!r.ok) return { ok: false, msg: "HTTP " + r.status };
        return r.json();
    }).then(res => {
        // 2026-08-10 D3: 控制命令下发成功后登记回比(防"假成功"——下发成功≠设备已执行)
        if (res && res.ok) _registerCmdVerify(cmd, paras);
        return res;
    }).catch(err => ({ ok: false, msg: "网络异常: " + err }));
}

// ============================================================
// 2026-08-10 D3: 命令下发后状态回比(防"假成功")
//   后端 publish_cmd 返回 ok 仅代表"华为云已接收"消息, 不代表设备已执行.
//   关键命令下发后登记期望状态, 在下次属性上报中比对目标字段:
//   - 控制命令 set_charge/set_discharge/set_balance/set_relay → charge_mos/
//     discharge_mos/balance_on 状态字段
//   - 参数命令 set_param → /api/params 回显对比(loadParams 内处理)
//   超时(15s)未变化 → 提示"设备未响应/执行失败".
// ============================================================
let _pendingCmdVerify = null;    // {cmd, checks:[{field,expect}], deadline}
const CMD_VERIFY_TIMEOUT_MS = 15000;

function _registerCmdVerify(cmd, paras) {
    const fieldMap = {
        "set_charge":    ["charge_mos"],
        "set_discharge": ["discharge_mos"],
        "set_balance":   ["balance_on"],
        "set_relay":     ["charge_mos", "discharge_mos"],
    };
    const fields = fieldMap[cmd];
    if (!fields || !paras) return;
    const checks = fields.map(f => {
        const v = paras.enable !== undefined ? paras.enable
                : (f === "charge_mos" ? paras.charge : paras.discharge);
        return { field: f, expect: v ? 1 : 0 };
    });
    _pendingCmdVerify = { cmd: cmd, checks: checks, deadline: Date.now() + CMD_VERIFY_TIMEOUT_MS };
    addLog("[命令] " + cmd + " 已下发, 等待设备确认(15s)...", "log-info");
}

function _checkCmdVerify(p) {
    if (!_pendingCmdVerify) return;
    const pv = _pendingCmdVerify;
    let allMatch = true;
    for (const c of pv.checks) {
        const cur = p[c.field];
        if (cur === undefined || cur === null || Number(cur) !== c.expect) { allMatch = false; break; }
    }
    if (allMatch) {
        addLog("[响应] ✅ " + pv.cmd + " 设备已确认执行(" + pv.checks.map(c => c.field + "=" + c.expect).join(",") + ")", "log-success");
        _pendingCmdVerify = null;
        return;
    }
    if (Date.now() > pv.deadline) {
        // 2026-08-21 修复: 提示真实原因——命令已下发成功(EMQX 通道), 但设备未确认执行。
        //   最常见根因: 设备处于保护/故障态(如 BQ76952 采样失效 fault=0x100000 → 保护全断),
        //   固件保护逻辑拒绝吸合 MOS, 属安全设计而非下发失败。
        let reason = "请检查设备故障状态/保护逻辑";
        try {
            const _f = Number(window.lastPayload && window.lastPayload.fault || 0);
            if (_f & 0x100000) reason = "设备处于 BQ76952 采样失效保护态(0x100000), 保护逻辑拒绝吸合 MOS, 请检查采样接线";
            else if (_f & 0x40000) reason = "设备绝缘保护/健康度告警中, 保护逻辑拒绝吸合 MOS";
            else if (_f & 0x00002) reason = "设备单体过压保护中, 禁止吸合 MOS";
            else if (_f & 0x00020) reason = "设备充电过流保护中, 禁止吸合 MOS";
            else if (_f & 0x00080) reason = "设备放电过流保护中, 禁止吸合 MOS";
            else if (_f !== 0) reason = "设备故障码 0x" + _f.toString(16) + " 保护中, 拒绝执行";
        } catch (e) {}
        addLog("[响应] ⚠️ " + pv.cmd + " 已下发但设备未确认("
               + pv.checks.map(c => c.field + " 未变为 " + c.expect).join(",")
               + ") — " + reason, "log-error");
        showToast(pv.cmd + " 已下发但设备未执行: " + reason, "error");
        _pendingCmdVerify = null;
    }
}

// ===== 2026-08-10 D3: 参数下发回比(set_param → /api/params 回显确认) =====
let _pendingParamVerify = null;   // {checks:[{key,value}], deadline}

function _registerParamVerify(checks) {
    if (!checks || !checks.length) return;
    _pendingParamVerify = { checks: checks, deadline: Date.now() + CMD_VERIFY_TIMEOUT_MS };
    addLog("[命令] 参数已下发 " + checks.length + " 项, 等待设备确认(15s)...", "log-info");
}

function _checkParamVerify(params) {
    if (!_pendingParamVerify || !params || typeof params !== "object") return;
    const pv = _pendingParamVerify;
    let allMatch = true;
    for (const c of pv.checks) {
        const meta = params[c.key];
        const cur = meta ? Number(meta.value) : null;
        if (cur === null || isNaN(cur) || cur !== Number(c.value)) { allMatch = false; break; }
    }
    if (allMatch) {
        addLog("[响应] ✅ 参数已确认生效(" + pv.checks.length + " 项)", "log-success");
        _pendingParamVerify = null;
        return;
    }
    if (Date.now() > pv.deadline) {
        addLog("[响应] ⚠️ 参数下发成功但设备未确认生效("
               + pv.checks.map(c => c.key + "=" + c.value).join(",")
               + "), 请检查固件是否含 messages/down 处理/烧录最新固件", "log-error");
        showToast("参数下发后设备未确认生效", "error");
        _pendingParamVerify = null;
    }
}

function toggleSwitch(el, name) {
    // ===== 2026-08-18 安全互锁(防止危险组合指令) =====
    // 规则:
    //   1) 充电 MOS 与 放电 MOS 互斥 —— 不能同时吸合(否则电池同时充放, 损坏电池)
    //   2) 设备处于保护/故障状态(error 级)时禁止开启任何 MOS(安全优先)
    //   3) 充放电总开关(MainRelay)是总闸, 与其下两个 MOS 分开控制, 不做互斥
    const willOn = !el.classList.contains("active");
    const FAULT_PROT_MASK = 0x00002 | 0x00020 | 0x00008 | 0x00080 | 0x00200 | 0x01000 | 0x00800 | 0x00400;
    const _curFault = (window.lastPayload && Number(window.lastPayload.fault || 0)) || 0;
    const _protActive = (_curFault & FAULT_PROT_MASK) !== 0;
    if (willOn && (name === "ChgMOS" || name === "DischgMOS") && _protActive) {
        addLog("[互锁] " + name + " 被阻止: 设备处于保护状态(fault=0x" + _curFault.toString(16) + ")", "log-error");
        showToast("安全互锁: 设备保护中(0x" + _curFault.toString(16) + "), 禁止开启 " + name, "error");
        return;
    }
    if (willOn && name === "ChgMOS" && switchState.DischgMOS) {
        addLog("[互锁] 开启充电前先关闭放电MOS(充放互斥)", "log-warn");
        showToast("安全互锁: 充电与放电互斥, 请先关闭放电MOS", "warn");
        return;
    }
    if (willOn && name === "DischgMOS" && switchState.ChgMOS) {
        addLog("[互锁] 开启放电前先关闭充电MOS(充放互斥)", "log-warn");
        showToast("安全互锁: 充电与放电互斥, 请先关闭充电MOS", "warn");
        return;
    }

    el.classList.toggle("active");
    // 2026-08-19: 分段开关——开关左侧的状态文字随 active 同步(开/关)
    const flag = el.previousElementSibling;
    if (flag && flag.classList.contains("switch-flag")) {
        flag.textContent = el.classList.contains("active") ? "开" : "关";
        flag.classList.toggle("on", el.classList.contains("active"));
    }
    const state = el.classList.contains("active");
    switchState[name] = state;
    addLog("[指令] " + name + " -> " + (state ? "ON" : "OFF"), "log-info");
    addOperationLog("切换 " + name + " -> " + (state ? "ON" : "OFF"), state ? "success" : "warn");

    // 根据 name 映射到实际命令
    let cmd, paras;
    if (name === "ChgMOS") {
        cmd = "set_charge"; paras = { enable: state };
    } else if (name === "DischgMOS") {
        cmd = "set_discharge"; paras = { enable: state };
    } else if (name === "Balance") {
        // ===== 2026-08-18 均衡控件统一: 开关与「均衡控制」区的模式按钮/弹窗共用一条下发链。
        //   开关 ON → 沿用当前模式(默认被动); 开关 OFF → 关闭均衡。
        //   避免此前开关(set_balance enable+mask)与模式按钮(mode)各自下发、状态不同步的冲突。
        const m = state ? ((balanceMode && balanceMode !== "off") ? balanceMode : "passive") : "off";
        // toggleBalance 内部会同步开关 UI + 下发 set_balance(带 mode), 此处直接委托
        toggleBalance(m);
        return;
    } else if (name === "MainRelay") {
        // 充放电总开关: 同时控制充放
        cmd = "set_relay"; paras = { charge: state, discharge: state };
    } else {
        return;
    }
    sendDeviceCmd(cmd, paras).then(res => {
        if (res.ok) {
            addLog("[响应] " + name + " 下发成功", "log-success");
        } else {
            // 失败时回滚 UI 状态, 避免界面与设备实际状态脱节
            el.classList.toggle("active");
            const flag2 = el.previousElementSibling;
            if (flag2 && flag2.classList.contains("switch-flag")) {
                flag2.textContent = el.classList.contains("active") ? "开" : "关";
                flag2.classList.toggle("on", el.classList.contains("active"));
            }
            switchState[name] = el.classList.contains("active");
            addLog("[响应] " + name + " 失败: " + (res.msg || ""), "log-error");
            showToast(name + " 下发失败: " + (res.msg || ""), "error");
        }
    });
}

// ============================================================
// 命令下发(原始 JSON 输入框)
// ============================================================
function sendCommand() {
    const input = $("cmdInput");
    if (!input) return;
    const raw = input.value.trim();
    if (!raw) {
        showToast("请输入命令", "warn");
        return;
    }
    let payload;
    try {
        payload = JSON.parse(raw);
    } catch (e) {
        // 不是 JSON, 当作 cmd 字符串
        payload = { cmd: raw };
    }
    // 如果是 set_param,走 /api/params
    // Bug修复: 兼容 params(带s) 和 paras(无s) 两种字段名, 华为云用 paras
    const paramsObj = payload.params || payload.paras;
    if (payload.cmd === "set_param" && paramsObj && paramsObj.key) {
        fetch("/api/params", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: paramsObj.key, value: paramsObj.value }),
        })
            .then(r => r.json())
            .then(res => {
                addLog("[发送] " + raw, "log-success");
                addLog("[响应] " + (res.ok ? "成功" : "失败: " + (res.msg || "")) + " " + (res.cmd ? JSON.stringify(res.cmd) : ""), res.ok ? "log-success" : "log-error");
                addOperationLog("下发参数 " + paramsObj.key + "=" + paramsObj.value + " -> " + (res.ok ? "成功" : "失败"), res.ok ? "success" : "error");
                showToast(res.ok ? "参数已下发" : "下发失败: " + res.msg, res.ok ? "success" : "error");
            })
            .catch(err => {
                addLog("[响应] 异常: " + err, "log-error");
                showToast("请求失败: " + err, "error");
            });
        input.value = "";
        return;
    }
    // 其他命令走 /api/cmd
    const cmd = payload.cmd || payload.command_name;
    if (!cmd) {
        showToast("未识别命令格式,请提供 cmd 字段", "error");
        return;
    }
    addLog("[发送] " + raw, "log-success");
    fetch("/api/cmd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
    })
        .then(r => r.json())
        .then(res => {
            addLog("[响应] " + (res.ok ? "成功" : "失败: " + (res.msg || "")) + " " + (res.payload ? JSON.stringify(res.payload) : ""), res.ok ? "log-success" : "log-error");
            addOperationLog("下发命令 " + cmd + " -> " + (res.ok ? "成功" : "失败"), res.ok ? "success" : "error");
            showToast(res.ok ? "命令已下发: " + cmd : "下发失败: " + res.msg, res.ok ? "success" : "error");
        })
        .catch(err => {
            addLog("[响应] 异常: " + err, "log-error");
            showToast("请求失败: " + err, "error");
        });
    input.value = "";
}

// 急停(关闭所有 MOS/FET)
function emergencyStop() {
    if (!confirm("⚠️ 确认执行急停? 所有 MOS/FET 将立即断开!")) return;
    addLog("[急停] !!! 所有 MOS/FET 关闭 !!!", "log-error");
    addOperationLog("执行急停, 断开所有开关", "error");
    document.querySelectorAll(".control-grid .toggle").forEach(t => t.classList.remove("active"));
    Object.keys(switchState).forEach(k => switchState[k] = false);
    // 下发 set_relay 断开所有 MOS/FET(实际急停)
    sendDeviceCmd("set_relay", { charge: false, discharge: false })
        .then(res => {
            if (res.ok) {
                addLog("[响应] 急停 MOS/FET 断开 成功", "log-success");
            } else {
                // 急停失败是严重安全问题, 必须强提示用户人工断电
                addLog("[响应] 急停下发失败: " + (res.msg || ""), "log-error");
                showToast("⚠️ 急停下发失败! 请人工断电!", "error");
            }
        });
    // 同时下发 clear_alarm 清除告警
    sendDeviceCmd("clear_alarm", {})
        .then(res => {
            addLog("[响应] 告警清除 " + (res.ok ? "成功" : "失败: " + (res.msg||"")), res.ok ? "log-success" : "log-error");
        });
    showToast("急停已执行", "error");
}

// ============================================================
// 充放电控制(真实下发 set_charge/set_discharge)
//   ✅ 优化: 命令下发 HTTP 200 成功后, 立刻做本地乐观更新 (lastPayload.charge_mos 改值)
//      这样用户点"停止充电"后, SOC卡状态秒变 "待机(充放MOS关)", 不等 180s 轮询
// ============================================================
function _applyLocalOptimisticMOS(chg, dsg) {
    if (!lastPayload) return;
    lastPayload.charge_mos = chg ? 1 : 0;
    lastPayload.discharge_mos = dsg ? 1 : 0;
    if (chg) lastPayload.charge_mode = 1;
    else if (dsg) lastPayload.charge_mode = 2;
    else lastPayload.charge_mode = 0;
    // 立刻刷新UI (handleBmsData 内部有去重 key=timestamp+current, 改 charge_mos 不会触发曲线重复push)
    handleBmsData(lastPayload);
}
function startCharging() {
    addLog("[指令] 开始充电 (充电MOS -> ON)", "log-success");
    addOperationLog("开始充电", "success");
    // 2026-08-21 修复(409): 后端充放互锁——放电MOS仍在吸合时 set_charge 返回 409
    //   ("安全互锁: 放电MOS仍在吸合"). 此处自动先关放电再开充电(与 toggleSwitch 互锁一致),
    //   避免用户点"开始充电"直接失败; 若关闭放电也失败则明确提示.
    const _dsgOn = !!(lastPayload && lastPayload.discharge_mos === 1);
    const _doCharge = () => sendDeviceCmd("set_charge", { enable: true }).then(res => {
        if (res.ok) {
            const t = document.querySelectorAll(".control-grid .toggle")[1];
            if (t) t.classList.add("active");
            switchState.ChgMOS = true;
            _applyLocalOptimisticMOS(true, (lastPayload?.discharge_mos === 1));
            addLog("[响应] 开始充电 已下发", "log-success");
            showToast("充电MOS已开启,等待设备状态上报", "success");
        } else {
            addLog("[响应] 开始充电 失败: " + (res.msg || ""), "log-error");
            showToast("开始充电失败: " + (res.msg || ""), "error");
        }
    });
    if (_dsgOn) {
        addLog("[互锁] 放电MOS仍在吸合, 自动先关闭放电再开启充电", "log-warn");
        sendDeviceCmd("set_discharge", { enable: false }).then(r => {
            if (r.ok) {
                switchState.DischgMOS = false;
                _applyLocalOptimisticMOS((lastPayload?.charge_mos === 1), false);
                return _doCharge();
            }
            addLog("[响应] 关闭放电MOS失败, 无法开启充电: " + (r.msg || ""), "log-error");
            showToast("关闭放电MOS失败, 无法开启充电", "error");
        });
    } else {
        _doCharge();
    }
}

function stopCharging() {
    addLog("[指令] 停止充电 (充电MOS -> OFF)", "log-error");
    addOperationLog("停止充电", "warn");
    sendDeviceCmd("set_charge", { enable: false }).then(res => {
        if (res.ok) {
            const t = document.querySelectorAll(".control-grid .toggle")[1];
            if (t) t.classList.remove("active");
            switchState.ChgMOS = false;
            _applyLocalOptimisticMOS(false, (lastPayload?.discharge_mos === 1));
            addLog("[响应] 停止充电 已下发", "log-success");
            showToast("充电MOS已关闭,网页状态已同步", "success");
        } else {
            addLog("[响应] 停止充电 失败: " + (res.msg || ""), "log-error");
            showToast("停止充电失败: " + (res.msg || ""), "error");
        }
    });
}

function startDischarging() {
    addLog("[指令] 开始放电 (放电MOS -> ON)", "log-success");
    addOperationLog("开始放电", "success");
    // 2026-08-21 修复(409): 后端充放互锁——充电MOS仍在吸合时 set_discharge 返回 409
    //   ("安全互锁: 充电MOS仍在吸合"). 与 startCharging 对称: 自动先关充电再开放电.
    const _chgOn = !!(lastPayload && lastPayload.charge_mos === 1);
    const _doDischarge = () => sendDeviceCmd("set_discharge", { enable: true }).then(res => {
        if (res.ok) {
            const t = document.querySelectorAll(".control-grid .toggle")[2];
            if (t) t.classList.add("active");
            switchState.DischgMOS = true;
            _applyLocalOptimisticMOS((lastPayload?.charge_mos === 1), true);
            addLog("[响应] 开始放电 已下发", "log-success");
            showToast("放电MOS已开启,等待设备状态上报", "success");
        } else {
            addLog("[响应] 开始放电 失败: " + (res.msg || ""), "log-error");
            showToast("开始放电失败: " + (res.msg || ""), "error");
        }
    });
    if (_chgOn) {
        addLog("[互锁] 充电MOS仍在吸合, 自动先关闭充电再开启放电", "log-warn");
        sendDeviceCmd("set_charge", { enable: false }).then(r => {
            if (r.ok) {
                switchState.ChgMOS = false;
                _applyLocalOptimisticMOS(false, (lastPayload?.discharge_mos === 1));
                return _doDischarge();
            }
            addLog("[响应] 关闭充电MOS失败, 无法开启放电: " + (r.msg || ""), "log-error");
            showToast("关闭充电MOS失败, 无法开启放电", "error");
        });
    } else {
        _doDischarge();
    }
}

function stopDischarging() {
    addLog("[指令] 停止放电 (放电MOS -> OFF)", "log-error");
    addOperationLog("停止放电", "warn");
    sendDeviceCmd("set_discharge", { enable: false }).then(res => {
        if (res.ok) {
            const t = document.querySelectorAll(".control-grid .toggle")[2];
            if (t) t.classList.remove("active");
            switchState.DischgMOS = false;
            _applyLocalOptimisticMOS((lastPayload?.charge_mos === 1), false);
            addLog("[响应] 停止放电 已下发", "log-success");
            showToast("放电MOS已关闭,网页状态已同步", "success");
        } else {
            addLog("[响应] 停止放电 失败: " + (res.msg || ""), "log-error");
            showToast("停止放电失败: " + (res.msg || ""), "error");
        }
    });
}

function rebootDevice() {
    if (!confirm("⚠️ 确定要重启 BMS 设备吗?\n\n设备将在 5 秒后重启,重启期间无法监控。")) return;
    addLog("[指令] 发送重启指令至设备...", "log-error");
    addOperationLog("发送重启指令", "error");
    fetch("/api/cmd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ cmd: "restart" }),
    })
        .then(r => {
            if (r.status === 401) { window.location.href = "/login"; return null; }
            return r.json();
        })
        .then(res => {
            if (!res) return;
            addLog("[响应] 重启 " + (res.ok ? "已下发" : "失败: " + res.msg), res.ok ? "log-success" : "log-error");
            if (res.ok) {
                document.getElementById("contentScroll").style.opacity = "0.4";
                setTimeout(() => { document.getElementById("contentScroll").style.opacity = "1"; }, 5000);
            } else {
                showToast("重启下发失败: " + (res.msg || ""), "error");
            }
        })
        .catch(err => addLog("[响应] 重启异常: " + err, "log-error"));
}

function resetFadeCap() {
    if (!confirm("⚠️ 确定要重置满电容量吗?\n\n这将重新校准 SOC 估算基准。")) return;
    addLog("[指令] 满电容量重置 -> 100%", "log-success");
    addOperationLog("校准满电容量", "success");
    // Bug修复: 原代码错误下发 clear_alarm(清除告警), 语义完全不符
    //          现改为下发 reset_params 重置参数(包含容量校准)
    sendDeviceCmd("reset_params", {}).then(res => {
        if (res.ok) {
            addLog("[响应] 满电容量重置 已下发", "log-success");
            showToast("满电容量重置指令已下发", "success");
        } else {
            addLog("[响应] 满电容量重置 失败: " + (res.msg || ""), "log-error");
            showToast("满电容量重置失败: " + (res.msg || ""), "error");
        }
    });
}

// ============================================================
// 均衡控制(真实下发 set_balance 命令)
// ============================================================
function toggleBalance(mode) {
    balanceMode = mode;
    const statusEl = $("balanceStatus");
    const msg = mode === "off" ? "关闭" : mode === "passive" ? "被动均衡" : "主动均衡";
    const icon = mode === "off" ? "❌" : mode === "passive" ? "🟢" : "🔵";
    if (statusEl) {
        statusEl.textContent = icon + " " + msg;
        statusEl.style.color = mode === "off" ? "var(--text-primary)" : "var(--accent-green)";
    }
    addOperationLog("均衡模式切换: " + msg, mode === "off" ? "warn" : "success");
    addLog("[均衡] 模式 -> " + msg, "log-info");
    // 同步主控开关
    const balanceToggle = document.querySelectorAll(".control-grid .toggle")[3];
    if (balanceToggle) {
        if (mode === "off") balanceToggle.classList.remove("active");
        else balanceToggle.classList.add("active");
        switchState.Balance = (mode !== "off");
    }
    // 真实下发 set_balance 命令(带 mode: off/passive/active, 被动 mask=0xFFFF 自动)
    const enable = (mode !== "off");
    const modeName = mode === "passive" ? "passive" : mode === "active" ? "active" : "off";
    sendDeviceCmd("set_balance", { enable: enable, mode: modeName, mask: 0xFFFF }).then(res => {
        addLog("[响应] 均衡 " + (res.ok ? "已下发" : "失败: " + (res.msg || "")),
               res.ok ? "log-success" : "log-error");
    });
    // 同时下发均衡阈值参数(辅助配置)
    const thresh = $("balanceThreshold") ? $("balanceThreshold").value : 5;
    fetch("/api/params", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ key: "balance_threshold_mv", value: parseInt(thresh, 10) }),
    }).then(r => r.json()).then(res => {
        addLog("[响应] 均衡阈值 " + (res.ok ? "已下发" : "失败"), res.ok ? "log-success" : "log-error");
    }).catch(() => {});
    // ===== 2026-08-08: 均衡起始SOC + 策略下发(设备端 v9 支持) =====
    const startSoc = $("balanceStartSOC") ? parseInt($("balanceStartSOC").value, 10) : 85;
    if (!isNaN(startSoc) && startSoc >= 0 && startSoc <= 100) {
        fetch("/api/params", {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: "balance_start_soc_pct", value: startSoc }),
        }).then(r => r.json()).then(res => {
            addLog("[均衡] 起始SOC " + (res.ok ? "已下发 " + startSoc + "%" : "失败"), res.ok ? "log-success" : "log-error");
        }).catch(() => {});
    }
    const stratEl = $("balanceStrategy");
    const stratMap = { "电压差触发": 0, "容量差触发": 1, "定时均衡": 2 };
    if (stratEl && stratMap[stratEl.value] !== undefined) {
        fetch("/api/params", {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: "balance_strategy", value: stratMap[stratEl.value] }),
        }).then(r => r.json()).then(res => {
            addLog("[均衡] 策略 " + (res.ok ? "已下发 " + stratEl.value : "失败"), res.ok ? "log-success" : "log-error");
        }).catch(() => {});
    }
}

// 均衡弹窗
function openBalanceModal() { const m = $("balanceModal"); if (m) m.classList.add("show"); }
function closeBalanceModal() { const m = $("balanceModal"); if (m) m.classList.remove("show"); }

// ===== 2026-08-18 布局优化: 控制面板"均衡控制"入口 → 滚动到下方均衡控制卡 =====
function scrollToBalance() {
    // 均衡控制卡片是 control 视图内最后一个 table-card(含 ⚖️ 均衡控制 标题)
    const cards = document.querySelectorAll("#view-control .table-card, section[data-view='control'] .table-card");
    let target = null;
    for (const c of cards) {
        if (c.querySelector(".table-title") && /均衡控制/.test(c.querySelector(".table-title").textContent)) {
            target = c;
            break;
        }
    }
    if (!target) { openBalanceModal(); return; }
    const sc = document.getElementById("contentScroll") || document.scrollingElement || document.documentElement;
    sc.scrollTo({ top: target.offsetTop - 20, behavior: "smooth" });
}
function sendBalanceCmd() {
    const mode = $("balanceMode") ? $("balanceMode").value : "被动均衡";
    const thresh = $("balanceThresholdModal") ? $("balanceThresholdModal").value : 0.02;
    const curr = $("balanceCurrent") ? $("balanceCurrent").value : 0.1;
    addLog('[发送] 均衡命令 mode=' + mode + ' threshold=' + thresh + 'V current=' + curr + 'A', "log-success");
    addOperationLog("下发均衡命令: " + mode + ", 阈值=" + thresh + "V, 电流=" + curr + "A", "info");
    // ===== 2026-08-18 均衡控件统一: 弹窗与主控开关/模式按钮共用同一状态 =====
    //   弹窗选择的模式同步到全局 balanceMode 与主控开关 UI, 再走 toggleBalance 统一下发,
    //   避免此前弹窗只发 enable+mask(不带 mode)、主控开关与弹窗状态各管各的冲突
    const modeName = (mode.indexOf("主动") >= 0) ? "active" : "passive";
    const balMask = (1 << Math.min(NUM_CELLS, 32)) - 1;
    // 同步全局模式 + 主控开关(与 toggleBalance 内部一致)
    balanceMode = modeName;
    const statusEl = $("balanceStatus");
    if (statusEl) {
        const icon = modeName === "passive" ? "🟢" : "🔵";
        statusEl.textContent = icon + " " + (modeName === "passive" ? "被动均衡" : "主动均衡");
        statusEl.style.color = "var(--accent-green)";
    }
    const balanceToggle = document.querySelectorAll(".control-grid .toggle")[3];
    if (balanceToggle) { balanceToggle.classList.add("active"); switchState.Balance = true; }
    // 统一下发(带 mode, 与 toggleBalance 同链)
    sendDeviceCmd("set_balance", { enable: true, mode: modeName, mask: balMask }).then(res => {
        addLog("[响应] 均衡 " + (res.ok ? "已下发" : "失败: " + (res.msg || "")), res.ok ? "log-success" : "log-error");
    });
    fetch("/api/params", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ key: "balance_threshold_mv", value: Math.round(parseFloat(thresh) * 1000) }),
    });
    closeBalanceModal();
    showToast("均衡命令已下发", "success");
}

// ============================================================
// 电池参数保存
// ============================================================
function saveBatteryParams() {
    const p = {
        type: $("batType") ? $("batType").value : "",
        capacity: $("batCapacity") ? $("batCapacity").value : "",
        voltage: $("batVoltage") ? $("batVoltage").value : "",
        series: $("batSeries") ? $("batSeries").value : NUM_CELLS,
        maxChg: $("maxChgCurrent") ? $("maxChgCurrent").value : "",
        maxDsg: $("maxDsgCurrent") ? $("maxDsgCurrent").value : "",
        algo: $("socAlgo") ? $("socAlgo").value : "",
        rate: $("sampleRate") ? $("sampleRate").value : "",
    };
    addOperationLog("电池参数: " + p.type + ", " + p.capacity + "Ah, " + p.series + "S, " + p.voltage + "V", "info");
    addLog("[参数] 保存电池配置: " + JSON.stringify(p), "log-success");
    // 下发关键参数到后端(映射到 PARAM_META)
    const promises = [];
    // ===== 2026-08-08 修复: 串数下发缺失, 网页改"串联节数"保存后设备不生效 =====
    // 2026-09-07: 上限 32→16(硬件 BQ76952 原生 3~16S, 固件按 BMS_HW_MAX_SERIES_NUM=16 校验)
    const seriesNum = parseInt(p.series, 10);
    if (seriesNum >= 1 && seriesNum <= 16) {
        promises.push(fetch("/api/params", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: "cell_series_num", value: seriesNum })
        }).then(r => r.json()).then(res => {
            if (res && res.ok) {
                addLog("[串数] 已下发 " + seriesNum + " 串, 等待设备切换算法并重新上报", "log-success");
                // 2026-08-09 修复: 记录用户配置串数——handleBmsData 每帧会收到后端
                //   cell_series_num(=设备上报值 16)并 applySeriesNum 覆盖, 导致"适配一秒又回16".
                //   用户下发成功后用 override 锁定, 直到设备确认切换(上报==用户配置)再解除.
                _userSeriesOverride = seriesNum;
                applySeriesNum(seriesNum);
                // 2026-08-09: 异步消息提示——msg_id 仅代表华为云已接收,
                //   设备执行结果以下次上报为准(串数/cells 长度变化); 持续无变化需查设备固件
                if (res.async) {
                    addLog("[串数] 提示: 命令为异步投递, 设备执行后会自动上报新串数(若长时间无变化请检查设备固件/串口)", "log-warning");
                }
            } else {
                addLog("[串数] 下发失败: " + ((res && res.msg) || "设备离线或超范围"), "log-error");
            }
        }));
    } else {
        addLog("[串数] 非法串数 " + p.series + " (仅支持 1~16 串, BQ76952 硬件上限)", "log-error");
        // 2026-08-09: 前端即时拦截提示(后端 /api/params 也有 min/max 双保险,
        //   输入框 max=16 仅 UI 提示, 手动输入超大值在此被拦截并明确告知)
        showToast("串联节数必须在 1~16 之间(当前 " + p.series + ")", "error");
        return;
    }
    if (p.maxChg) {
        promises.push(fetch("/api/params", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ key: "chg_oc_prot_ma", value: parseInt(p.maxChg, 10) * 1000 }) }));
    }
    if (p.maxDsg) {
        promises.push(fetch("/api/params", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ key: "dsg_oc_prot_ma", value: parseInt(p.maxDsg, 10) * 1000 }) }));
    }
    // ===== 2026-08-08: 电池类型 + SOC 算法下发(设备端 v8 支持, 实时切换估算算法) =====
    const typeIdx = { "磷酸铁锂 (LFP)": 0, "三元锂 (NCM)": 1, "钛酸锂 (LTO)": 2, "铅酸": 3 };
    // ===== 2026-08-09: SOC 算法 5 档映射(与固件 soc_algo 0~4 完全一致) =====
    //   0=AEKF 1=纯安时积分 2=OCV查表 3=MCC-EKF(默认) 4=UKF
    const algoIdx = { "MCC-EKF(默认最优)": 3, "AEKF双卡尔曼": 0, "纯安时积分": 1, "开路电压法": 2, "UKF无迹卡尔曼": 4 };
    if (p.type && typeIdx[p.type] !== undefined) {
        promises.push(fetch("/api/params", {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: "battery_type", value: typeIdx[p.type] })
        }).then(r => r.json()).then(res => {
            addLog("[电池类型] " + (res.ok ? "已下发 " + p.type : "下发失败: " + (res.msg || "")), res.ok ? "log-success" : "log-error");
        }));
    }
    if (p.algo && algoIdx[p.algo] !== undefined) {
        promises.push(fetch("/api/params", {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: "soc_algo", value: algoIdx[p.algo] })
        }).then(r => r.json()).then(res => {
            addLog("[SOC算法] " + (res.ok ? "已下发 " + p.algo : "下发失败: " + (res.msg || "")), res.ok ? "log-success" : "log-error");
        }));
    }
    if (p.capacity && parseFloat(p.capacity) > 0) {
        promises.push(fetch("/api/params", {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: "cell_capacity_mah", value: Math.round(parseFloat(p.capacity) * 1000) })
        }));
    }
    Promise.all(promises).then(() => {
        addLog("[响应] 电池参数已下发", "log-success");
        showToast("电池参数已保存并下发", "success");
        // 下发成功后延迟回显, 让网页更新为设备生效后的配置
        setTimeout(() => { try { loadParams(); } catch (e) {} }, 1500);
        // 2026-08-10 修复: 下发后同步刷新设备管理表格(电芯类型/容量等列,
        //   原逻辑只刷新参数表单, 设备管理表只有手动点"刷新"才更新)
        setTimeout(() => { try { loadDevices(); } catch (e) {} }, 2500);
    }).catch(err => {
        addLog("[响应] 参数下发异常: " + err, "log-error");
    });
}

// ============================================================
// 阈值保存 (与后端 PARAM_META 一致, 对应 bms_config.h 宏值)
// ============================================================
/* 字段映射: HTML id -> (后端 key, 倍率 -> 后端单位) */
const THRESHOLD_MAP = [
    ["thOverVolt",              "cell_ov_prot_mv",      1000],  // V -> mV
    ["thOverVoltWarn",          "cell_ov_warn_mv",      1000],
    ["thUnderVolt",             "cell_uv_prot_mv",      1000],
    ["thUnderVoltWarn",         "cell_uv_warn_mv",      1000],
    ["thDvWarn",                "cell_dv_warn_mv",        1],  // mV -> mV
    ["thOverCurrent",           "dsg_oc_prot_ma",       1000],  // A  -> mA (放电)
    ["thOverCurrentWarn",       "dsg_oc_warn_ma",       1000],
    ["thChgOverCurrent",        "chg_oc_prot_ma",       1000],  // A  -> mA (充电)
    ["thChgOverCurrentWarn",    "chg_oc_warn_ma",       1000],
    ["thMaxTemp",               "temp_ot_prot_dc",        10],  // °C -> 0.1℃
    ["thMaxTempWarn",           "temp_ot_warn_dc",        10],
    ["thMinTemp",               "temp_ut_prot_dc",        10],
    ["thMinTempWarn",           "temp_ut_warn_dc",        10],
    ["thSocLowWarn",            "soc_low_warn_pct",        1],  // %  -> %
    ["thSohLowWarn",            "soh_low_warn_pct",        1],
    ["balanceThreshold",        "balance_threshold_mv",   1],  // mV -> mV
    ["balanceStopMv",           "balance_stop_mv",        1],
];

function saveThresholds() {
    const tasks = [];
    const saved = [];
    const sentParams = [];   // 2026-08-10 D3: 记录下发参数, 用于回比确认
    THRESHOLD_MAP.forEach(([id, key, mul]) => {
        const el = $(id);
        if (!el) return;
        const v = el.value;
        if (v === "" || v === null || isNaN(parseFloat(v))) return;
        const val = Math.round(parseFloat(v) * mul);
        saved.push(id + "=" + v);
        sentParams.push({ key: key, value: val });
        tasks.push(fetch("/api/params", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: key, value: val })
        }).then(r => r.json()));
    });
    addOperationLog("阈值保存: " + saved.join(" · "), "info");
    addLog("[阈值] 保存: " + saved.join(" · "), "log-success");
    Promise.all(tasks).then(results => {
        const fails = results.filter(r => !r.ok).map(r => r.msg).join(";");
        if (fails) {
            addLog("[响应] 部分阈值下发失败: " + fails, "log-error");
            showToast("部分阈值下发失败: " + fails, "error");
        } else {
            addLog("[响应] 阈值已保存并尝试下发", "log-success");
            showToast("阈值已保存并下发 (设备离线会在下次上线时同步)", "success");
            // 2026-08-10 D3: 登记参数回比(loadParams 回显时确认设备是否已生效)
            _registerParamVerify(sentParams);
            // 2026-08-08 修复: 下发成功后延迟回显, 让网页更新为设备生效的新阈值
            setTimeout(() => { try { loadParams(); } catch (e) {} }, 1500);
        }
    }).catch(err => {
        addLog("[响应] 阈值保存异常: " + err, "log-error");
    });
}

function resetThresholds() {
    fetch("/api/params").then(r => r.json()).then(params => {
        if (!params || typeof params !== "object") return;
        let count = 0;
        THRESHOLD_MAP.forEach(([id, key, mul]) => {
            const el = $(id);
            const meta = params[key];
            if (!el || !meta) return;
            const def = (meta.default != null) ? meta.default : meta.value;
            // 浮点数友好处理: mul=10 时保留 1 位, mul=1000 时 V 保留 2-3 位
            let v = def / mul;
            if (mul === 1000) v = Number(v.toFixed(3));
            else if (mul === 10) v = Number(v.toFixed(1));
            else v = Number(v.toFixed(2));
            el.value = v;
            count++;
        });
        addLog("[阈值] 已恢复主控默认 (" + count + " 项)", "log-success");
        showToast("已恢复主控默认", "success");
    }).catch(err => showToast("恢复失败: " + err, "error"));
}

// ============================================================
// CSV 导出(单体数据 + 告警) - 旧版快照导出,保留
// ============================================================
function exportCSV() {
    let csv = "\uFEFF"; // BOM for Excel
    // 第一部分: 单体数据
    csv += "=== 单体数据 ===\n";
    csv += "时间,电芯编号,电压(V),内阻(mΩ),温差(°C),SOC(%),状态\n";
    const now = new Date().toLocaleString("zh-CN");
    for (let i = 0; i < cellData.length; i++) {
        const c = cellData[i];
        const status = c.status === "error" ? "异常" : c.status === "warn" ? "预警" : "正常";
        csv += now + ",#" + (i + 1) + "," + c.voltage.toFixed(3) + "," + c.irq.toFixed(1) + "," + Math.abs(c.temp - 30).toFixed(1) + "," + c.soc.toFixed(1) + "," + status + "\n";
    }
    // 第二部分: 系统摘要
    csv += "\n=== 系统摘要 ===\n";
    csv += "时间,总电压(V),总电流(A),SOC(%),SOH(%),功率(W),故障码\n";
    if (lastPayload) {
        const v = Number(lastPayload.pack_v || 0) / 1000;
        const a = Number(lastPayload.current || 0) / 1000;
        csv += now + "," + v.toFixed(2) + "," + a.toFixed(2) + "," + (lastPayload.soc || 0) + "," + (lastPayload.soh || 0) + "," + (v * a).toFixed(1) + ",0x" + Number(lastPayload.fault || 0).toString(16).toUpperCase() + "\n";
    }
    // 第三部分: 告警列表
    csv += "\n=== 告警列表 ===\n";
    csv += "时间,故障码,描述,等级,状态\n";
    currentAlerts.forEach(a => {
        csv += a.time + ",0x" + a.code.toString(16).toUpperCase() + "," + a.desc + "," + a.level + "," + (a.acknowledged ? "已确认" : "未确认") + "\n";
    });
    if (currentAlerts.length === 0) csv += now + ",--,无告警,--,--\n";

    const blob = new Blob([csv], { type: "text/csv;charset=utf-8;" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "BMS_snapshot_" + new Date().toISOString().slice(0, 10) + ".csv";
    a.click();
    URL.revokeObjectURL(url);
    addOperationLog("数据已导出CSV(快照)", "success");
    addLog("[导出] CSV 快照已生成", "log-success");
    showToast("CSV 快照已导出", "success");
}

// P2: 真实历史数据 CSV 导出(从后端 /api/export.csv 下载)
function exportRealCSV() {
    // 使用当前选中的历史范围
    const rangeMap = { "1h": "1h", "6h": "6h", "24h": "24h", "7d": "7d" };
    const range = rangeMap[historyRange] || "24h";
    addLog("[导出] 拉取 " + range + " 真实历史数据...", "log-info");
    // 直接触发浏览器下载
    const url = "/api/export.csv?range=" + range + "&limit=50000";
    const a = document.createElement("a");
    a.href = url;
    a.download = "BMS_history_" + range + ".csv";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    addOperationLog("导出 " + range + " 历史数据 CSV", "success");
    showToast("已导出 " + range + " 历史数据", "success");
}

/** 2026-08-10 #29: 自定义日期区间 CSV 导出(支持任意历史区间, 不再限于预设范围) */
function exportCsvRange() {
    const sEl = $("csvStart"), eEl = $("csvEnd");
    let s = sEl ? sEl.value : "";
    let e = eEl ? eEl.value : "";
    if (!s || !e) {
        const today = new Date();
        const weekAgo = new Date(today.getTime() - 7 * 86400000);
        if (!s) s = _fmtIsoDate(weekAgo);
        if (!e) e = _fmtIsoDate(today);
        if (sEl) sEl.value = s;
        if (eEl) eEl.value = e;
    }
    if (s > e) { showToast("起始日期不能晚于结束日期", "warn"); return; }
    addLog("[导出] 拉取 " + s + " ~ " + e + " 历史数据...", "log-info");
    const url = "/api/export.csv?start=" + encodeURIComponent(s) + "&end=" + encodeURIComponent(e) + "&limit=50000";
    const a = document.createElement("a");
    a.href = url;
    a.download = "BMS_history_" + s + "_" + e + ".csv";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    addOperationLog("导出 " + s + " ~ " + e + " 历史数据 CSV", "success");
    showToast("已导出 " + s + " ~ " + e + " 历史数据", "success");
}

/** 版本号比较 "1.0.1" vs "1.0.225": 返回 1=a>b, -1=a<b, 0=相同 */
function compareVersion(a, b) {
    const pa = String(a || "0").split(".").map(n => parseInt(n, 10) || 0);
    const pb = String(b || "0").split(".").map(n => parseInt(n, 10) || 0);
    for (let i = 0; i < 3; i++) {
        const x = pa[i] || 0, y = pb[i] || 0;
        if (x !== y) return x > y ? 1 : -1;
    }
    return 0;
}

// ============================================================
// P2-1: OTA 远程升级 (2026-08-13: 两段式 "检查→确认→升级")
// ============================================================
function otaCheck() {
    addLog("[OTA] 触发 OTA 检查更新...", "log-info");
    addOperationLog("OTA 检查更新", "info");
    const statusEl = $("otaStatus");
    if (statusEl) { statusEl.textContent = "检查中..."; statusEl.style.color = "var(--accent-yellow)"; }

    // 1. 先让设备侧做一次检查(固件人工确认模式下只缓存新版本, 不自动升级)
    sendDeviceCmd("ota_check").then(() => {
        // 2. 从后端拉远程版本信息(version.json 匿名可访问)
        return fetch("/api/ota/version.json").then(r => r.json()).catch(() => null);
    }).then(meta => {
        const remoteVer = meta && meta.version ? String(meta.version) : "";
        const remoteUrl = meta && meta.url ? String(meta.url) : "";
        const curVer = _cachedFirmware && _cachedFirmware !== "--" ? _cachedFirmware : "1.0.1";

        if (!remoteVer) {
            if (statusEl) { statusEl.textContent = "检查失败(无版本信息)"; statusEl.style.color = "var(--accent-red)"; }
            addLog("[OTA] 检查失败: 未获取到远程版本信息", "log-error");
            return;
        }

        // 3. 版本对比: 远程 <= 当前 → 已是最新
        if (compareVersion(remoteVer, curVer) <= 0) {
            if (statusEl) { statusEl.textContent = "已是最新版本"; statusEl.style.color = "var(--accent-green)"; }
            addLog("[OTA] 已是最新版本 (当前 v" + curVer + ")", "log-success");
            showToast("已是最新版本 v" + curVer, "success");
            return;
        }

        // 4. 发现新版本 → 弹窗确认, 确认后才下发升级
        if (statusEl) { statusEl.textContent = "发现新版本 v" + remoteVer; statusEl.style.color = "var(--accent-yellow)"; }
        addLog("[OTA] 发现新版本 v" + remoteVer + " (当前 v" + curVer + ")", "log-info");
        const ok = confirm("🔔 发现新版本 v" + remoteVer + "\n当前版本: v" + curVer +
                           "\n\n是否立即升级?\n(升级期间设备将断开 1-5 分钟, 完成后自动重启)");
        if (!ok) {
            addLog("[OTA] 用户取消升级", "log-info");
            showToast("已取消升级", "info");
            return;
        }
        if (remoteUrl) {
            _otaTargetVer = remoteVer;   // 记录目标版本, 供升级后回联验证
            otaUpgrade(remoteUrl, remoteVer);   // 确认后带后端固件 URL + 目标版本执行升级
        } else {
            addLog("[OTA] 远程 URL 为空, 请手动填写", "log-warn");
            showToast("未获取到固件 URL, 请手动填写后点立即升级", "warn");
        }
    }).catch(err => {
        if (statusEl) { statusEl.textContent = "检查失败"; statusEl.style.color = "var(--accent-red)"; }
        addLog("[OTA] 检查失败: " + err, "log-error");
    });
}

function otaUpgrade(prefillUrl, targetVer) {
    const urlInput = $("otaUrl");
    if (prefillUrl && urlInput) { urlInput.value = prefillUrl; }
    const url = urlInput ? urlInput.value.trim() : "";
    if (!url) {
        showToast("请输入固件下载 URL", "warn");
        return;
    }
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
        showToast("URL 必须以 http:// 或 https:// 开头", "warn");
        return;
    }
    if (!confirm("⚠️ 确认立即升级?\n\n固件 URL: " + url + "\n升级期间设备将断开 1-5 分钟,完成后自动重启。")) return;
    addLog("[OTA] 触发强制升级: " + url, "log-error");
    addOperationLog("OTA 强制升级: " + url, "error");
    const statusEl = $("otaStatus");
    if (statusEl) { statusEl.textContent = "升级中..."; statusEl.style.color = "var(--accent-yellow)"; }
    sendDeviceCmd("ota_upgrade", { url: url }).then(res => {
        if (res.ok) {
            // targetVer 仅 otaCheck 流程已知; 强制升级(手动 URL)传 null → 成功判定走"版本变化"兜底
            _otaTargetVer = (typeof targetVer !== "undefined" && targetVer) ? targetVer : null;
            startOtaTrack();   // 进入状态机: 指令已下发 → 等待设备回联验证
            if (statusEl) { statusEl.textContent = "指令已下发·设备升级中"; statusEl.style.color = "var(--accent-blue)"; }
            addLog("[OTA] 升级指令已下发, 等待设备重启回联验证", "log-success");
            showToast("指令已下发,设备升级中(预计1-5分钟),完成后自动校验", "success");
        } else {
            if (statusEl) { statusEl.textContent = "下发失败"; statusEl.style.color = "var(--accent-red)"; }
            addLog("[OTA] 升级下发失败: " + (res.msg || ""), "log-error");
            showToast("下发失败: " + (res.msg || ""), "error");
        }
    });
}

// ============================================================
// P2-5: 固件发布(2026-08-16 新增, admin-only)
//   上传 bms.bin + 版本号 → POST /api/ota/publish(后端落盘 firmware/ + 写 version.json)
//   设备下次检查(12h 自动 / 前端"检查更新")即发现新版本升级
// ============================================================
function otaPublish() {
    if (!window._IS_ADMIN) { showToast("仅管理员可发布固件", "error"); return; }
    const fileEl = $("otaPublishFile");
    const verEl = $("otaPublishVer");
    const file = fileEl && fileEl.files && fileEl.files[0];
    const version = verEl ? verEl.value.trim() : "";
    if (!file) { showToast("请选择固件文件(bms.bin)", "warn"); return; }
    if (!/\.bin$/i.test(file.name)) { showToast("固件必须是 .bin 文件", "warn"); return; }
    if (!version) { showToast("请填写版本号(如 1.0.3)", "warn"); return; }
    if (!confirm("确认发布固件 v" + version + "?\n文件: " + file.name + " (" + (file.size/1024).toFixed(0) + " KB)\n\n发布后设备下次检查将自动升级。\n(请确认 bms_config.h 版本号已同步并编译)") ) return;

    addLog("[OTA] 上传固件 v" + version + " (" + file.name + ") 发布中...", "log-info");
    const fd = new FormData();
    fd.append("file", file);
    fd.append("version", version);
    fetch("/api/ota/publish", { method: "POST", body: fd })
        .then(r => r.json())
        .then(d => {
            if (d.ok) {
                addLog("[OTA] 固件 v" + version + " 发布成功(" + d.size + " B)", "log-success");
                addOperationLog("OTA 固件发布 v" + version, "info");
                showToast("固件 v" + version + " 已发布, 设备将自动升级", "success");
                const hint = $("otaPublishVer");
                if (hint) hint.value = "";
                if (fileEl) fileEl.value = "";
            } else {
                addLog("[OTA] 发布失败: " + (d.msg || ""), "log-error");
                showToast("发布失败: " + (d.msg || ""), "error");
            }
        })
        .catch(err => {
            addLog("[OTA] 发布请求异常: " + err, "log-error");
            showToast("发布失败: " + err, "error");
        });
}

// ============================================================
// P2-6: 服务器固件信息展示(2026-08-16 新增)
//   拉取 /api/ota/info, 在 OTA 面板显示服务器上的固件版本/大小/发布时间,
//   便于发布前后核对 version.json 与固件是否一致
// ============================================================
function otaRefreshServerInfo() {
    const el = $("otaServerInfo");
    if (!el) return;
    el.textContent = "服务器固件: 加载中...";
    fetch("/api/ota/info").then(r => r.json()).then(d => {
        if (!d || !d.ok) { el.textContent = "服务器固件: 获取失败"; return; }
        const ver = d.version ? "v" + d.version : "--";
        const size = d.size != null ? (d.size / 1024).toFixed(0) + " KB" : "--";
        const bt = d.build_time || "--";
        const exists = d.firmware_exists ? "" : " ⚠️ 服务器无固件";
        el.textContent = "服务器固件: " + ver + " · " + size + " · 构建 " + bt + exists;
    }).catch(() => { el.textContent = "服务器固件: 获取失败"; });
}

// ============================================================
// P2-3: OTA 升级状态机 (2026-08-14)
//   问题: 旧逻辑 sendDeviceCmd 返回 ok 仅代表"云端已收消息", 不代表设备已执行,
//         状态停在"升级指令已下发", 用户无法知晓是否升级成功.
//   方案: 企业级分段状态机(参考 AWS IoT Jobs / 阿里云 IoT / 华为IoTDA 软件升级任务):
//         - ① 指令下发: 云端 ack(前端固有, 已确认)
//         - ② 设备下载: 设备回报 ota_stage=downloading(已确认, 带进度%)
//         - ③ 校验写入: 设备回报 ota_stage=verifying(镜像写入+SHA256校验通过, 已确认)
//         - ④ 重启验证: 设备回报 rebooting + 新固件回联版本匹配目标(已确认)
//   固件随属性上报 otaStage/otaProgress(2026-08-14 新增), ② ③ 由此从"推测"升级为"设备已确认",
//   阶段圆点显示绿色✓角标; 终态: 升级成功(版本匹配) / 升级超时失败(超时不回联或版本未变) / 设备回报 failed.
// ============================================================
let _otaTrack = null;          // {startTs, deadline, terminal, timer}
let _otaTargetVer = null;      // 目标版本(otaCheck 流程已知; 强制升级可能为 null)
let _otaPrevVer = "--";        // 升级前版本(强制升级且无目标版本时用于"版本变化"判定)
let _otaDeviceOnline = true;   // 设备在线(由 mqtt_status 维护)
let _otaDeviceStage = "";      // 设备回报的 OTA 阶段(downloading/verifying/rebooting/failed/idle)
let _otaProgress = 0;          // 设备回报的下载进度(0~100)
let _otaError = "";            // 设备回报的失败错误码(无失败为空串)
let _otaStageArmed = false;    // 门控: 本轮升级是否已进入下载(见到 downloading 后才采信 failed, 防旧失败残留误杀)
const OTA_TIMEOUT_MS = 8 * 60 * 1000;   // 8 分钟超时(文档: 升级断开 1-5 分钟)

function _otaSetStatus(text, color) {
    const el = $("otaStatus");
    if (el) { el.textContent = text; el.style.color = color; }
}

function _otaRenderStepper(activeStep, terminal, opts) {
    // terminal: null | "done" | "fail"
    // opts.confirmedDone : 已完成且"设备已确认"的步骤下标数组(绿✓)
    // opts.confirmedActive: 正在进行且"设备已确认"的步骤下标(-1 表示无)
    opts = opts || {};
    const confirmedDone = opts.confirmedDone || [];
    const confirmedActive = (typeof opts.confirmedActive === "number") ? opts.confirmedActive : -1;
    const steps = document.querySelectorAll("#otaStepper .ota-step");
    if (!steps.length) return;
    steps.forEach((s, i) => {
        s.classList.remove("active", "done", "fail", "confirmed");
        s.removeAttribute("data-confirmed");
        if (terminal === "done") {
            s.classList.add("done", "confirmed");
            s.setAttribute("data-confirmed", "1");   // 终态成功 = 全链路设备已确认, 显示绿✓
        } else if (terminal === "fail") {
            s.classList.add(i === 0 ? "done" : "fail");   // 已知指令已发, 后续阶段未达成
        } else {
            if (confirmedDone.indexOf(i) >= 0) {
                s.classList.add("done", "confirmed");
                s.setAttribute("data-confirmed", "1");
            } else if (i === confirmedActive) {
                s.classList.add("active", "confirmed");
                s.setAttribute("data-confirmed", "1");
            } else if (i < activeStep) {
                s.classList.add("done");
            } else if (i === activeStep) {
                s.classList.add("active");
            }
        }
    });
}

function _otaStopTimer() {
    if (_otaTrack && _otaTrack.timer) { clearInterval(_otaTrack.timer); _otaTrack.timer = null; }
}

function _otaTick() {
    if (!_otaTrack) return;
    _checkOtaVerify();   // 事件可能未触发, 定时器兜底回验
    if (!_otaTrack || _otaTrack.terminal) return;
    const now = Date.now();
    const remain = Math.max(0, Math.ceil((_otaTrack.deadline - now) / 1000));
    const cd = $("otaCountdown");
    if (cd) cd.textContent = "剩余 " + remain + "s";
    if (now > _otaTrack.deadline) {
        _otaTrack.terminal = "fail";
        _otaRenderStepper(0, "fail");
        _otaSetStatus("❌ 升级超时/失败", "var(--accent-red)");
        addLog("[OTA] 升级超时: 设备未在 " + (OTA_TIMEOUT_MS / 60000) + " 分钟内回联或版本未变化, 请检查固件/网络", "log-error");
        showToast("OTA 升级超时/失败, 请检查设备", "error");
        addOperationLog("OTA 升级超时/失败", "error");
        _otaStopTimer();
    }
}

function startOtaTrack() {
    _otaPrevVer = (_cachedFirmware && _cachedFirmware !== "--") ? _cachedFirmware : "--";
    _otaDeviceStage = "";      // 复位设备阶段(等待本次升级的设备回报)
    _otaProgress = 0;
    _otaError = "";           // 复位失败错误码
    _otaStageArmed = false;   // 复位门控(本轮尚未见到设备下载)
    _otaTrack = { startTs: Date.now(), deadline: Date.now() + OTA_TIMEOUT_MS, terminal: null, timer: null };
    _otaRenderStepper(1, null);          // ① 已确认, ② 进行中
    _otaSetStatus("指令已下发·设备升级中", "var(--accent-blue)");
    const cd = $("otaCountdown"); if (cd) cd.textContent = "剩余 " + (OTA_TIMEOUT_MS / 1000) + "s";
    const tv = $("otaTargetVer"); if (tv) tv.textContent = _otaTargetVer || "—";
    addLog("[OTA] 进入升级状态机: 等待设备下载/校验/重启后回联验证", "log-info");
    _otaStopTimer();
    _otaTrack.timer = setInterval(_otaTick, 1000);
}

function _checkOtaVerify() {
    if (!_otaTrack || _otaTrack.terminal) return;
    const curVer = (_cachedFirmware && _cachedFirmware !== "--") ? _cachedFirmware : null;
    const stage = (_otaDeviceStage || "").toLowerCase();

    // ---- 设备阶段回验: 把 ②设备下载 / ③校验写入 从"推断"升级为"设备已确认" ----
    if (stage === "failed") {
        if (!_otaStageArmed) return;   // 上一轮残留 / 本轮尚未进入下载 → 忽略, 避免误杀新一轮升级
        _otaTrack.terminal = "fail";
        _otaRenderStepper(0, "fail");
        const errCode = _otaError || "UNKNOWN";
        _otaSetStatus("❌ 升级失败(" + errCode + ")", "var(--accent-red)");
        const cd = $("otaCountdown"); if (cd) cd.textContent = "";
        addLog("[OTA] ❌ 设备回报升级失败, 错误码=" + errCode, "log-error");
        showToast("OTA 升级失败(" + errCode + ")", "error");
        addOperationLog("OTA 升级失败(" + errCode + ")", "error");
        _otaStopTimer();
        return;
    }
    // 见到设备真实活动阶段 → 置门控, 此后 failed 才被采信(防止旧 FAILED 残留误判)
    if (stage === "downloading" || stage === "verifying" || stage === "rebooting") {
        _otaStageArmed = true;
    }
    if (stage === "downloading") {
        // ② 设备下载: 设备已确认正在下载(带实时进度)
        _otaRenderStepper(1, null, { confirmedActive: 1 });
        _otaSetStatus("设备下载中 " + (_otaProgress || 0) + "%", "var(--accent-blue)");
    } else if (stage === "verifying") {
        // ③ 校验写入: 镜像已写入+SHA256校验通过, 设备已确认
        _otaRenderStepper(2, null, { confirmedDone: [1], confirmedActive: 2 });
        _otaSetStatus("校验写入中…", "var(--accent-blue)");
    } else if (stage === "rebooting") {
        // ④ 重启验证: 已标记 pending, 即将重启
        _otaRenderStepper(3, null, { confirmedDone: [1, 2] });
        _otaSetStatus("重启验证中…", "var(--accent-blue)");
    }
    // idle / 空(尚未收到设备回报): 维持 startOtaTrack 初始态(①done ②active 推断), 等待回报或版本回联

    // ---- 成功判定: 设备在线 且 (目标版本已知且匹配 | 或 版本相对升级前发生变化) ----
    let success = false;
    if (_otaDeviceOnline && curVer) {
        if (_otaTargetVer && curVer === _otaTargetVer) success = true;
        else if (!_otaTargetVer && _otaPrevVer && _otaPrevVer !== "--" && curVer !== _otaPrevVer) success = true;
    }
    if (success) {
        _otaTrack.terminal = "done";
        _otaRenderStepper(3, "done");
        _otaSetStatus("✅ 升级成功 v" + curVer, "var(--accent-green)");
        const cd = $("otaCountdown"); if (cd) cd.textContent = "";
        addLog("[OTA] ✅ 升级成功: 设备已回联, 固件版本 " + curVer + (_otaTargetVer ? (" (目标 v" + _otaTargetVer + ")") : ""), "log-success");
        showToast("OTA 升级成功 v" + curVer, "success");
        addOperationLog("OTA 升级成功 v" + curVer, "success");
        _otaStopTimer();
    }
}

// ============================================================
// P2-2: 报表生成(日报/周报/月报/自定义范围)
// ============================================================
let reportChart = null;

function initReportChart() {
    const ctx = $("reportChart");
    if (!ctx) return;
    reportChart = new Chart(ctx.getContext("2d"), {
        type: "line",
        data: {
            labels: [],
            datasets: [
                { label: "SOC 均值(%)",  data: [], borderColor: COLOR.blue,  backgroundColor: hexRgba(COLOR.blue,0.08), fill: true, borderWidth: 1.6, tension: 0.35, pointRadius: 2 },
                { label: "总压 均值(V)", data: [], borderColor: COLOR.cyan,  borderWidth: 1.5, tension: 0.3, pointRadius: 1.5, yAxisID: "y1" },
                { label: "电流 均值(A)", data: [], borderColor: COLOR.green, borderWidth: 1.5, tension: 0.3, pointRadius: 1.5, yAxisID: "y2" },
            ],
        },
        options: {
            responsive: true, maintainAspectRatio: false,
            animation: { duration: 250 },
            interaction: { mode: "index", intersect: false },
            plugins: { legend: { labels: { color: COLOR.textSec, font: { size: 10 }, boxWidth: 10 } } },
            scales: {
                x:  { grid: { color: themeGridColor() }, ticks: { color: themeMutedColor(), font: { size: 9 }, maxTicksLimit: 10 } },
                y:  { min: 0, max: 100, grid: { color: themeGridColor() }, ticks: { color: COLOR.blue,  font: { size: 9 } } },
                y1: { position: "right", grid: { drawOnChartArea: false }, ticks: { color: COLOR.cyan, font: { size: 9 } } },
                y2: { position: "right", grid: { drawOnChartArea: false }, ticks: { color: COLOR.green, font: { size: 9 } } },
            },
        },
    });
}

let lastReportData = null;

function hexRgba(hex, alpha) {
    // #rrggbb -> rgba(r,g,b,a)
    const h = hex.replace("#","");
    if (h.length !== 6) return hex;
    const r = parseInt(h.substring(0,2),16);
    const g = parseInt(h.substring(2,4),16);
    const b = parseInt(h.substring(4,6),16);
    return "rgba(" + r + "," + g + "," + b + "," + alpha + ")";
}

function _fmtIsoDate(d) {
    const y = d.getFullYear();
    const m = String(d.getMonth() + 1).padStart(2, "0");
    const day = String(d.getDate()).padStart(2, "0");
    return y + "-" + m + "-" + day;
}

/** 切换报表类型时: monthly -> input type=month, daily/weekly -> type=date, custom -> 显示 start/end */
function onReportTypeChange() {
    const type = $("reportType") ? $("reportType").value : "daily";
    const dateWrap = $("reportDateWrap");
    const custWrap = $("reportCustomWrap");
    const dateEl = $("reportDate");
    if (type === "custom") {
        if (dateWrap) dateWrap.style.display = "none";
        if (custWrap) custWrap.style.display = "";
        // 默认: 近 7 天
        const today = new Date();
        const weekAgo = new Date(today.getTime() - 7*86400000);
        if ($("reportEnd"))   $("reportEnd").value   = _fmtIsoDate(today);
        if ($("reportStart")) $("reportStart").value = _fmtIsoDate(weekAgo);
    } else {
        if (custWrap) custWrap.style.display = "none";
        if (dateWrap) dateWrap.style.display = "";
        if (!dateEl) return;
        if (type === "yearly") {
            // 年报: 仅输入年份(YYYY)
            dateEl.type = "text";
            dateEl.placeholder = "YYYY";
            if (!/^\d{4}$/.test(dateEl.value || "")) dateEl.value = String(new Date().getFullYear());
        } else if (type === "monthly" || type === "quarterly") {
            // 月报/季报: 用月份输入(YYYY-MM); 后端对 quarterly 自动取所在季度
            // Bug4 修复: 部分浏览器(如旧版 Chromium/Firefox 移动端) 不支持 type=month
            //   先尝试切换为 month, 失败则保持 date 但用 YYYY-MM 填充并提示用户手动补 "-01"
            try { dateEl.type = "month"; } catch (_) {}
            if (dateEl.type !== "month") {
                // 不支持 type=month => 退回 type=date, 显示当月 1 号 (用户可以改成任意日期)
                dateEl.type = "date";
                if (!dateEl.value || dateEl.value.length !== 10) {
                    const d = new Date();
                    dateEl.value = d.getFullYear() + "-" + String(d.getMonth()+1).padStart(2,"0") + "-01";
                }
            } else {
                if (dateEl.value && dateEl.value.length >= 10) dateEl.value = dateEl.value.substring(0, 7);
                if (!dateEl.value) {
                    const d = new Date();
                    dateEl.value = d.getFullYear() + "-" + String(d.getMonth()+1).padStart(2,"0");
                }
            }
        } else {
            try { dateEl.type = "date"; } catch (_) {}
            if (dateEl.value && dateEl.value.length === 7) dateEl.value = dateEl.value + "-01";
            if (!dateEl.value) dateEl.value = _fmtIsoDate(new Date());
        }
    }
}

/** 快捷日期: today / yesterday / week / month */
function setReportQuick(kind) {
    const today = new Date();
    const selT = $("reportType");
    const dateEl = $("reportDate");
    const startEl = $("reportStart");
    const endEl = $("reportEnd");
    if (kind === "today") {
        selT.value = "daily"; onReportTypeChange();
        dateEl.value = _fmtIsoDate(today);
    } else if (kind === "yesterday") {
        selT.value = "daily"; onReportTypeChange();
        const y = new Date(today.getTime() - 86400000);
        dateEl.value = _fmtIsoDate(y);
    } else if (kind === "week") {
        selT.value = "custom"; onReportTypeChange();
        const dow = today.getDay() || 7;   // 周一是1, 周日是7
        const monday = new Date(today.getTime() - (dow - 1) * 86400000);
        const sunday = new Date(monday.getTime() + 6 * 86400000);
        startEl.value = _fmtIsoDate(monday);
        endEl.value   = _fmtIsoDate(sunday);
    } else if (kind === "month") {
        selT.value = "monthly"; onReportTypeChange();
        dateEl.value = today.getFullYear() + "-" + String(today.getMonth()+1).padStart(2,"0");
    } else if (kind === "quarter") {
        selT.value = "quarterly"; onReportTypeChange();
        dateEl.value = today.getFullYear() + "-" + String(today.getMonth()+1).padStart(2,"0");
    } else if (kind === "year") {
        selT.value = "yearly"; onReportTypeChange();
        dateEl.value = String(today.getFullYear());
    }
    loadReport();
}

function loadReport() {
    const type = $("reportType") ? $("reportType").value : "daily";
    let params = "type=" + encodeURIComponent(type);
    if (type === "custom") {
        const s = $("reportStart") ? $("reportStart").value : "";
        const e = $("reportEnd")   ? $("reportEnd").value   : "";
        params += "&start=" + encodeURIComponent(s) + "&end=" + encodeURIComponent(e);
    } else {
        let date = $("reportDate") ? $("reportDate").value : "";
        // monthly/quarterly -> 后端接受 YYYY-MM, daily/weekly -> YYYY-MM-DD, yearly -> YYYY
        if ((type === "monthly" || type === "quarterly") && date && date.length >= 10) date = date.substring(0, 7);
        if (type !== "monthly" && type !== "quarterly" && type !== "yearly" && date && date.length === 7) date = date + "-01";
        if (date) params += "&date=" + encodeURIComponent(date);
    }
    addLog("[报表] 生成 " + ({daily:"日报", weekly:"周报", monthly:"月报", quarterly:"季报", yearly:"年报", custom:"自定义范围"}[type] || type), "log-info");
    if ($("reportChartStatus")) $("reportChartStatus").textContent = "生成中…";
    fetch("/api/report?" + params)
        .then(r => r.json())
        .then(res => {
            if (!res.ok) {
                showToast("报表生成失败: " + (res.msg || ""), "error");
                if ($("reportChartStatus")) $("reportChartStatus").textContent = "失败";
                return;
            }
            lastReportData = res;
            renderReport(res);
        })
        .catch(err => {
            addLog("[报表] 异常: " + err, "log-error");
            showToast("报表生成异常: " + err, "error");
            if ($("reportChartStatus")) $("reportChartStatus").textContent = "异常";
        });
}

function renderReport(res) {
    const summaryEl = $("reportSummary");
    const alertsEl  = $("reportAlerts");
    const tagEl     = $("reportRangeTag");
    const hintEl    = $("reportBucketHint");
    const stEl      = $("reportChartStatus");
    const typeLabel = { daily: "日报", weekly: "周报", monthly: "月报", quarterly: "季报", yearly: "年报", custom: "自定义范围" }[res.type] || res.type;
    if (tagEl) tagEl.textContent = (res.start_date && res.end_date) ? ("· " + res.start_date + " ~ " + res.end_date) : ("· " + (res.date || ""));
    if (res.empty || !res.summary) {
        if (summaryEl) summaryEl.innerHTML = '<div style="color:var(--accent-yellow);padding:22px;text-align:center;grid-column:1/-1"><strong>' + htmlEsc(res.msg || "该时段无数据") + "</strong><br><span style='font-weight:400;font-size:12px;opacity:.8'>（" + typeLabel + " · " + htmlEsc(res.start_date || "") + " ~ " + htmlEsc(res.end_date || "") + "）</span></div>";
        if (alertsEl)  alertsEl.innerHTML  = '<div style="color:var(--text-muted);padding:6px">—</div>';
        if (hintEl)    hintEl.textContent   = "";
        if (stEl)      stEl.textContent     = "无数据";
        if (reportChart) {
            reportChart.data.labels = [];
            reportChart.data.datasets.forEach(ds => ds.data = []);
            reportChart.update("none");
        }
        return;
    }
    const s = res.summary;
    const cards = [
        { label: "数据点数",    value: s.count,                                  color: "var(--accent-blue)",   icon: "📊" },
        { label: "SOC 均值",    value: s.soc_avg + "%",                          color: "var(--accent-cyan)",   icon: "🔋" },
        { label: "SOC 区间",    value: s.soc_min + "% ~ " + s.soc_max + "%",     color: "var(--accent-cyan)",   icon: "📈" },
        { label: "SOH 均值",    value: s.soh_avg + "%",                          color: "var(--accent-purple)", icon: "💚" },
        { label: "总压 均值",   value: (s.pack_v_avg / 1000).toFixed(2) + " V",   color: "var(--accent-blue)",   icon: "⚡" },
        { label: "总压 区间",   value: (s.pack_v_min/1000).toFixed(2) + " ~ " + (s.pack_v_max/1000).toFixed(2) + " V", color: "var(--accent-blue)", icon: "±" },
        { label: "电流 均值",   value: (s.current_avg / 1000).toFixed(2) + " A",  color: "var(--accent-green)",  icon: "🔌" },
        { label: "电流 范围",   value: (s.current_min/1000).toFixed(2) + " ~ " + (s.current_max/1000).toFixed(2) + " A", color: "var(--accent-green)", icon: "↔" },
        { label: "充电量",      value: s.charge_ah + " Ah",                       color: "var(--accent-yellow)", icon: "⬆" },
        { label: "放电量",      value: s.discharge_ah + " Ah",                    color: "var(--accent-orange)", icon: "⬇" },
        { label: "温度 区间",   value: s.temp_min + " ℃ ~ " + s.temp_max + " ℃",  color: "var(--accent-red)",    icon: "🌡" },
        { label: "故障 记录",   value: s.fault_count,                             color: s.fault_count > 0 ? "var(--accent-red)" : "var(--accent-green)", icon: "⚠" },
    ];
    if (summaryEl) {
        summaryEl.innerHTML = cards.map(c =>
            '<div style="padding:12px;background:var(--bg-secondary);border-radius:8px;border:1px solid var(--border)">' +
            '<div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:4px">' +
              '<div style="font-size:11px;color:var(--text-secondary)">' + c.label + '</div>' +
              '<div style="font-size:14px">' + (c.icon || "") + '</div>' +
            '</div>' +
            '<div style="font-size:15px;font-weight:700;color:' + c.color + ';line-height:1.2">' + c.value + '</div>' +
            '</div>'
        ).join("");
    }
    // 图表 (pack_v / current 转成 V / A)
    if (reportChart) {
        reportChart.data.labels = (res.buckets || []).map(b => b.time);
        reportChart.data.datasets[0].data = (res.buckets || []).map(b => b.soc_avg || null);
        reportChart.data.datasets[1].data = (res.buckets || []).map(b => (b.pack_v_avg == null || b.pack_v_avg === 0) ? null : (Number(b.pack_v_avg) / 1000.0));
        reportChart.data.datasets[2].data = (res.buckets || []).map(b => (b.current_avg == null) ? null : (Number(b.current_avg) / 1000.0));
        // y1/y2 轴自适应
        const packArr = reportChart.data.datasets[1].data.filter(x => x != null);
        const curArr  = reportChart.data.datasets[2].data.filter(x => x != null);
        if (packArr.length) {
            const mn = Math.min(...packArr), mx = Math.max(...packArr);
            const pad  = Math.max(1, (mx - mn) * 0.2);
            reportChart.options.scales.y1.min = +(mn - pad).toFixed(2);
            reportChart.options.scales.y1.max = +(mx + pad).toFixed(2);
        } else { reportChart.options.scales.y1.min = undefined; reportChart.options.scales.y1.max = undefined; }
        if (curArr.length) {
            const mn = Math.min(...curArr), mx = Math.max(...curArr);
            const pad  = Math.max(0.5, (mx - mn) * 0.2);
            reportChart.options.scales.y2.min = +(mn - pad).toFixed(2);
            reportChart.options.scales.y2.max = +(mx + pad).toFixed(2);
        } else { reportChart.options.scales.y2.min = undefined; reportChart.options.scales.y2.max = undefined; }
        reportChart.update("none");
    }
    if (hintEl) {
        const total = (res.buckets || []).length;
        const hasData = (res.buckets || []).some(b => b.count > 0);
        const byMonth = (res.buckets || []).some(b => /^\d{4}-\d{2}$/.test(b.time || ""));
        const byHour = (res.buckets || []).some(b => (b.time || "").indexOf(":") >= 0 && (b.time || "").length <= 6);
        hintEl.textContent = total ? ("（" + total + " 个" + (byMonth ? "月" : byHour ? "小时" : "日") + "聚合点 · " + (hasData ? "有数据" : "空") + "）") : "";
    }
    if (stEl) stEl.textContent = "完成 · " + (s.count || 0) + " 条";
    // 告警统计 (新增 hex + 累计百分比条)
    if (alertsEl) {
        if (!res.alerts || res.alerts.length === 0) {
            alertsEl.innerHTML = '<div style="color:var(--accent-green);padding:10px;background:rgba(34,197,94,0.06);border:1px solid rgba(34,197,94,0.15);border-radius:6px">✓ 该时段无告警（' + typeLabel + ' · ' + htmlEsc(res.start_date || '') + ' ~ ' + htmlEsc(res.end_date || '') + '）</div>';
        } else {
            const total = res.alerts.reduce((a, b) => a + Number(b.count || 0), 0) || 1;
            alertsEl.innerHTML = '<div style="display:grid;gap:6px">' + res.alerts.map(a => {
                const pct = ((a.count / total) * 100).toFixed(0);
                return '<div style="padding:8px 10px;background:var(--bg-secondary);border:1px solid var(--border);border-radius:6px">' +
                    '<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:4px">' +
                    '<span><span style="color:var(--accent-red)">●</span> ' +
                    '<strong style="margin:0 6px">' + htmlEsc(a.name || '') + '</strong>' +
                    '<span style="font-family:monospace;color:var(--text-mut);font-size:11px">' + htmlEsc(a.hex || ('0x' + (1 << a.code).toString(16).toUpperCase())) + '</span>' +
                    '</span>' +
                    '<span style="font-weight:700;color:var(--accent-yellow)">×' + a.count + ' (' + pct + '%)</span>' +
                    '</div>' +
                    '<div style="height:4px;background:var(--bg-primary);border-radius:2px;overflow:hidden">' +
                    '<div style="height:100%;width:' + pct + '%;background:linear-gradient(90deg,var(--accent-yellow),var(--accent-red))"></div>' +
                    '</div></div>';
            }).join("") + '</div>';
        }
    }
    addLog("[报表] " + typeLabel + " 已生成 · 数据 " + (s.count || 0) + " 条 · 告警 " + (res.alerts || []).length + " 类", "log-success");
}

function exportReportCSV() {
    if (!lastReportData) {
        showToast("请先点击「🔍 生成」报表", "warn");
        return;
    }
    const r = lastReportData;
    const typeLabel = { daily:"日报", weekly:"周报", monthly:"月报", custom:"自定义范围"}[r.type] || r.type;
    let csv = "\uFEFF";
    csv += "BMS " + typeLabel + " - " + (r.start_date || r.date || "") + " ~ " + (r.end_date || r.date || "") + "\n\n";
    csv += "=== 汇总统计 ===\n";
    csv += "指标,数值\n";
    if (r.summary) {
        const s = r.summary;
        csv += "数据点数," + s.count + "\n";
        csv += "SOC均值(%)," + s.soc_avg + "\n";
        csv += "SOC最低(%)," + s.soc_min + "\n";
        csv += "SOC最高(%)," + s.soc_max + "\n";
        csv += "SOH均值(%)," + s.soh_avg + "\n";
        csv += "总压均值(V)," + (s.pack_v_avg/1000).toFixed(3) + "\n";
        csv += "总压最低(V)," + (s.pack_v_min/1000).toFixed(3) + "\n";
        csv += "总压最高(V)," + (s.pack_v_max/1000).toFixed(3) + "\n";
        csv += "电流均值(A)," + (s.current_avg/1000).toFixed(3) + "\n";
        csv += "电流最低(A)," + (s.current_min/1000).toFixed(3) + "\n";
        csv += "电流最高(A)," + (s.current_max/1000).toFixed(3) + "\n";
        csv += "充电量(Ah)," + s.charge_ah + "\n";
        csv += "放电量(Ah)," + s.discharge_ah + "\n";
        csv += "温度最低(℃)," + s.temp_min + "\n";
        csv += "温度最高(℃)," + s.temp_max + "\n";
        csv += "故障记录数," + s.fault_count + "\n";
    }
    csv += "\n=== 时段明细 ===\n";
    csv += "时段,SOC均值(%),总压均值(V),电流均值(A),最高温(℃),数据条数\n";
    (r.buckets || []).forEach(b => {
        csv += (b.key || b.time) + "," +
               (b.soc_avg || "") + "," +
               ((b.pack_v_avg == null) ? "" : (Number(b.pack_v_avg)/1000.0).toFixed(3)) + "," +
               ((b.current_avg == null) ? "" : (Number(b.current_avg)/1000.0).toFixed(3)) + "," +
               (b.temp_max ?? "") + "," +
               (b.count || 0) + "\n";
    });
    csv += "\n=== 告警统计 ===\n";
    csv += "HEX码,bit位,描述,次数\n";
    if (!r.alerts || r.alerts.length === 0) {
        csv += "--,--,无告警,0\n";
    } else {
        r.alerts.forEach(a => {
            csv += (a.hex || ("0x"+(1<<a.code).toString(16).toUpperCase())) + "," +
                   a.code + "," + (a.name||"") + "," + a.count + "\n";
        });
    }
    const blob = new Blob([csv], { type: "text/csv;charset=utf-8;" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "BMS_" + r.type + "_" + (r.start_date || r.date || "") + "_" + (r.end_date || "") + ".csv";
    a.click();
    URL.revokeObjectURL(url);
    showToast("报表已导出 CSV", "success");
    addLog("[报表] CSV 已导出", "log-success");
}

/** 导出 PDF: 直接用浏览器 window.print 打印到 PDF (报表卡片专色打印) */
function exportReportPDF() {
    if (!lastReportData) {
        showToast("请先点击「🔍 生成」报表", "warn");
        return;
    }
    const card = $("reportCard");
    if (!card) { showToast("未找到报表容器", "error"); return; }
    // 注入临时 @media print 样式 (通过 style#__printStyle)
    let style = document.getElementById("__reportPrintStyle");
    if (!style) {
        style = document.createElement("style");
        style.id = "__reportPrintStyle";
        document.head.appendChild(style);
    }
    style.textContent = [
        "@media print {",
        "  body * { visibility:hidden !important; }",
        "  #reportCard, #reportCard * { visibility:visible !important; }",
        "  #reportCard { position:absolute !important; left:0; top:0; width:100%; box-shadow:none !important; }",
        "  .command-btn, .device-select, .threshold-input, input, select, button { display:none !important; }",
        "}",
    ].join("\n");
    setTimeout(() => {
        try { window.print(); }
        finally {
            // 500ms 后移除样式(避免阻塞)
            setTimeout(() => { if (style) style.textContent = ""; }, 800);
        }
    }, 50);
}

/** 下载该时间范围对应的原始 CSV (走 /api/history) */
function exportReportRaw() {
    if (!lastReportData || !lastReportData.start_date || !lastReportData.end_date) {
        showToast("请先生成报表", "warn");
        return;
    }
    const start = new Date(lastReportData.start_date + "T00:00:00");
    const end   = new Date(lastReportData.end_date   + "T23:59:59");
    const minutes = Math.max(1, Math.round((end.getTime() - start.getTime()) / 60000));
    const url = "/api/export.csv?minutes=" + encodeURIComponent(minutes);
    const a = document.createElement("a");
    a.href = url;
    a.download = "BMS_raw_" + (lastReportData.start_date || "") + "_" + (lastReportData.end_date || "") + ".csv";
    a.click();
    showToast("原始 CSV 已开始下载", "success");
    addLog("[报表] 原始 CSV 下载: " + minutes + " 分钟", "log-success");
}

// ============================================================
// P2-3: 多设备管理 (16列,参数与主控 bms_config.h 严格一致)
// ============================================================
// ============================================================
// 电池组切换(deviceSelect 下拉): 下发 switch_battery + 刷新参数面板
// 配置来自 /api/devices 返回的 groups 字段(与后端 BATTERY_GROUPS 一致)
// ============================================================
let _batteryGroups = [];   // [{group, name, short, series, capacity_mah}]

// 2026-08-11: 侧边栏"设备"字段实时回显 —— 根据当前选中设备解析设备元信息
//   主设备(_currentDeviceId="")→ 取 BMS-001; 从设备(如 BMS002)→ 按 device_id 后缀匹配
function currentDeviceMeta() {
    if (_currentDeviceId) {
        const dev = _deviceList.find(d => {
            const id = String(d.device_id || "");
            return id.endsWith("_" + _currentDeviceId) || id === _currentDeviceId
                || id.indexOf(_currentDeviceId) >= 0;
        });
        if (dev) return dev;
    }
    // 主设备: 优先 name 含 BMS-001, 否则取列表第一个
    return _deviceList.find(d => (d.name || "").indexOf("BMS-001") >= 0)
        || _deviceList[0] || null;
}

// 更新侧边栏页脚"设备"名称; 并在 bms_info 实时固件抵达前用设备列表静态值预填"固件"
function updateSidebarDevice() {
    const dev = currentDeviceMeta();
    const sfDev = $("sfDev");
    if (sfDev) sfDev.textContent = dev ? (dev.name || "--") : "--";
    // 固件: 仅当尚无实时值(_cachedFirmware 仍为空/--)时预填, 避免覆盖 bms_info 实时值
    if (!_cachedFirmware || _cachedFirmware === "--") {
        const fw = dev ? (dev.firmware || "--") : "--";
        if (fw && fw !== "--") {
            _cachedFirmware = fw;
            const sfFw = $("sfFw");
            if (sfFw) sfFw.textContent = fw;
            const otaCur = $("otaCurVer");
            if (otaCur) otaCur.textContent = fw;
        }
    }
}

function onDeviceSwitch() {
    const sel = $("deviceSelect");
    if (!sel) return;
    const label = (sel.options[sel.selectedIndex] || {}).text || "";
    // 用选中项文字匹配配置表(兼容 "BMS-001 主电池组" 与 "BMS-001" 两种格式)
    const g = _batteryGroups.find(x => label.indexOf(x.short) >= 0) ||
              _batteryGroups.find(x => x.name === label);
    if (!g) {
        addLog("[电池组] 未找到配置: " + label, "log-warning");
        return;
    }
    // 2026-08-19 修复(C8): 下拉切换即下发 switch_battery 属于误触高风险操作——
    //   用户只是浏览下拉列表(滚动选中)就可能让设备切换电池组算法。
    //   增加确认弹窗, 只有确认后才真正下发。
    if (!window.confirm("确认下发电池组切换到 " + g.short + " (" + g.series + "S " +
                        g.capacity_mah + "mAh)?\n\n设备会切换串数并重新校准, 请确认。")) {
        // 用户取消: 恢复下拉为当前生效设备, 不下发
        addLog("[电池组] 已取消切换: " + g.short, "log-info");
        try {
            const cur = _batteryGroups.find(x => x.group === _currentGroup) || _batteryGroups[0];
            if (cur && sel) sel.value = cur.short || cur.name;
        } catch (e) {}
        return;
    }
    // 2026-08-09 多设备: 记录当前选中设备, 轮询按设备加载数据
    //   设备 ID 格式与后端一致: 主设备=空(走 /api/status), 从设备=BMS00X 后缀
    _currentDeviceId = (g.group && g.group > 1) ? ("BMS00" + g.group) : "";
    // 2026-08-11: 切换电池组时同步刷新侧边栏"设备"名称
    try { updateSidebarDevice(); } catch (e) {}
    // 避免误触发: 与当前显示串数一致时仍允许重发(设备端幂等), 直接下发
    addLog("[电池组] 切换至 " + g.short + " → " + g.series + "S " + g.capacity_mah + "mAh", "log-info");
    fetch("/api/cmd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
            cmd: "switch_battery",
            paras: { group: g.group, series: g.series, capacity_mah: g.capacity_mah },
        }),
    })
        .then(r => r.json())
        .then(res => {
            if (res.ok) {
                addLog("[电池组] 已下发, 等待设备切换算法并重新上报…", "log-success");
                showToast("已下发切换 " + g.short + " (" + g.series + "S)", "success");
                // 设备切换串数后立即刷新参数面板(串数/阈值回显)
                setTimeout(() => { try { loadParams(); } catch (e) {} }, 1200);
            } else {
                addLog("[电池组] 下发失败: " + (res.msg || ""), "log-error");
                showToast("切换失败: " + (res.msg || ""), "error");
            }
        })
        .catch(err => {
            addLog("[电池组] 请求异常: " + err, "log-error");
            showToast("切换请求失败", "error");
        });
}

function loadDevices() {
    addLog("[设备] 加载设备列表...", "log-info");
    fetch("/api/devices")
        .then(r => r.json())
        .then(res => {
            const tbody = $("deviceTableBody");
            if (!tbody) return;
            if (!res.devices || res.devices.length === 0) {
                tbody.innerHTML = '<tr><td colspan="16" style="color:var(--text-muted);padding:20px;text-align:center">未配置设备</td></tr>';
                return;
            }
            // 缓存电池组配置表(供 deviceSelect 切换时下发 switch_battery)
            if (Array.isArray(res.groups)) {
                _batteryGroups = res.groups;
            }
            // 2026-08-11: 缓存设备列表并刷新侧边栏"设备/固件"字段
            if (Array.isArray(res.devices)) {
                _deviceList = res.devices;
                try { updateSidebarDevice(); } catch (e) {}
            }
            tbody.innerHTML = res.devices.map(d => {
                // 2026-08-09 修复: 渲染设备列表**不再调用 applySeriesNum(d.series)**——
                //   多设备 series 为 16/8/16, map 遍历时依次应用导致全局串数 8↔16 反复循环,
                //   且覆盖用户下发的配置(30s 轮询每次都在改). 设备表格直接用 d.series 显示,
                //   全局串数唯一来源 = handleBmsData 的实时配置 cell_series_num.
                // 1. 在线状态
                const statusHtml = d.online
                    ? '<span class="cell-indicator normal">在线</span>'
                    : '<span class="cell-indicator error">离线</span>';
                // 2. 最后上报时间
                const lastSeen = d.last_seen
                    ? new Date(d.last_seen < 1e12 ? d.last_seen * 1000 : d.last_seen).toLocaleString("zh-CN", { hour12: false })
                    : "--";
                // 3. S×P 配置 (串联×并联)
                const sp = (d.series || "-") + "×" + (d.parallel || "-");
                // 4. 电压范围: 标称/满充/截止 (来自 bms_config.h 宏)
                const vRange = (d.nominal_v || "--") + " / " + (d.full_v || "--") + " / " + (d.cutoff_v || "--");
                // 5. 当前SOC + 颜色条
                const soc = Number(d.cur_soc || 0);
                const socColor = soc <= 15 ? "var(--accent-red)" : soc <= 30 ? "var(--accent-yellow)" : "var(--accent-green)";
                const socHtml = '<span style="color:' + socColor + ';font-weight:600">' + soc.toFixed(1) + '%</span>' +
                    '<div style="height:4px;background:var(--bg-input);border-radius:2px;margin-top:3px;overflow:hidden">' +
                    '<div style="height:100%;width:' + Math.min(100, Math.max(0, soc)) + '%;background:' + socColor + '"></div>' +
                    '</div>';
                // 6. 循环次数
                const cyc = Number(d.cycle_count || 0);
                const cycColor = cyc >= 1000 ? "var(--accent-red)" : cyc >= 500 ? "var(--accent-yellow)" : "var(--text-primary)";
                // 7. 产品型号 + SN (两行显示)  (2026-08-10 F4: 设备可控字段转义)
                const modelSn = '<div style="font-weight:600">' + htmlEsc(d.hardware || "--") + '</div>' +
                    '<div style="font-family:monospace;font-size:10px;color:var(--text-muted);margin-top:2px">SN: ' + htmlEsc(d.sn || "--") + '</div>';
                // 8. 主控芯片 + 通信协议 (tooltip显示协议详情)
                const chipProto = '<div>' + htmlEsc(d.chip || "--") + '</div>' +
                    '<div style="font-size:10px;color:var(--text-muted);margin-top:2px" title="通信协议">' + htmlEsc(d.protocol || "--") + '</div>';
                return '<tr>' +
                    // 1. 设备名称
                    '<td><strong>' + htmlEsc(d.name || "--") + '</strong></td>' +
                    // 2. 产品型号 / SN
                    '<td style="font-size:12px">' + modelSn + '</td>' +
                    // 3. 设备 ID
                    '<td style="font-family:monospace;font-size:11px" title="' + htmlEsc(d.device_id || "") + '">' +
                        htmlEsc(d.device_id ? (String(d.device_id).length > 18 ? String(d.device_id).slice(0, 16) + "…" : d.device_id) : "--") +
                    '</td>' +
                    // 4. 部署区域
                    '<td>' +
                        '<div>' + htmlEsc(d.region || "--") + '</div>' +
                        (d.cloud_region ? '<div style="font-size:10px;color:var(--text-muted);margin-top:2px">云: ' + htmlEsc(d.cloud_region) + '</div>' : "") +
                    '</td>' +
                    // 5. 在线状态
                    '<td>' + statusHtml + '</td>' +
                    // 6. 配置(S×P)
                    '<td style="text-align:center;font-weight:600">' + sp + '</td>' +
                    // 7. 电芯类型
                    '<td style="font-size:11px;text-align:center">' + htmlEsc(d.cell_type || "--") + '</td>' +
                    // 8. 容量(Ah)
                    '<td style="text-align:center;font-weight:600">' + htmlEsc(d.capacity_ah || "--") + '</td>' +
                    // 9. 电压范围(标/满/切)
                    '<td style="font-family:monospace;font-size:10px" title="标称/满充/截止 (V)">' + vRange + '</td>' +
                    // 10. 当前SOC + 条
                    '<td style="min-width:90px">' + socHtml + '</td>' +
                    // 11. 循环次数
                    '<td style="text-align:center;color:' + cycColor + ';font-weight:600">' + cyc + '</td>' +
                    // 12. 固件版本
                    '<td style="font-family:monospace;font-size:11px" title="固件版本 ' + htmlEsc(d.firmware || "") + '">v' + htmlEsc(d.firmware || "--") + '</td>' +
                    // 13. 主控芯片 + 协议
                    '<td style="font-size:11px">' + chipProto + '</td>' +
                    // 14. 厂商
                    '<td style="font-size:11px">' + htmlEsc(d.manufacturer || "--") + '</td>' +
                    // 15. 最后上报
                    '<td style="font-size:11px;white-space:nowrap">' + lastSeen + '</td>' +
                    // 16. 操作
                    '<td style="white-space:nowrap">' +
                        '<button class="command-btn" style="padding:3px 8px;font-size:11px" onclick="rebootDevice()">重启</button> ' +
                        '<button class="command-btn" style="padding:3px 8px;font-size:11px" onclick="sendDeviceCmd(\'get_info\').then(r=>addLog(\'[设备] 查询信息 \'+(r.ok?\'成功\':\'失败\'),r.ok?\'log-success\':\'log-error\'))">查询</button>' +
                    '</td>' +
                    '</tr>';
            }).join("");
            addLog("[设备] 已加载 " + res.count + " 台设备 (参数与主控 bms_config.h 已同步)", "log-success");
        })
        .catch(err => {
            addLog("[设备] 加载失败: " + err, "log-error");
            showToast("设备列表加载失败", "error");
        });
}

// ============================================================
// P2-4: SOH 趋势分析(简化版,替代 LSTM)
// 基于历史数据线性外推,估算月衰减率与剩余寿命
// ============================================================
let sohTrendChart = null;

function initSohTrendChart() {
    const ctx = $("sohTrendChart");
    if (!ctx) return;
    sohTrendChart = new Chart(ctx.getContext("2d"), {
        type: "line",
        data: {
            labels: [],
            datasets: [
                { label: "SOH 实测(%)", data: [], borderColor: COLOR.purple, backgroundColor: "rgba(139,92,246,0.08)", borderWidth: 1.5, fill: true, tension: 0.4, pointRadius: 0 },
                { label: "衰减外推(%)", data: [], borderColor: COLOR.yellow, borderWidth: 1, borderDash: [5, 5], tension: 0.4, pointRadius: 0, fill: false },
                { label: "寿命终点阈值(%)", data: [], borderColor: COLOR.red, borderWidth: 1, borderDash: [2, 3], tension: 0, pointRadius: 0, fill: false },
            ],
        },
        options: {
            responsive: true, maintainAspectRatio: false,
            animation: { duration: 300 },
            interaction: { mode: "index", intersect: false },
            plugins: { legend: { labels: { color: COLOR.textSec, font: { size: 10 }, boxWidth: 10 } } },
            scales: {
                x: { grid: { color: themeGridColor() }, ticks: { color: themeMutedColor(), font: { size: 9 }, maxTicksLimit: 8 } },
                y: { min: 0, max: 100, grid: { color: themeGridColor() }, ticks: { color: "#94a3b8", font: { size: 9 } } },
            },
        },
    });
}

// P2-4: SOH 衰减分析全局状态
let lastSohTrendRange = "24h";   /* 供 5min 定时刷新保持上次按钮 */
let _sohRangeTimer = null;
let _sohAbort = null;
function loadSohTrend(range, el) {
    range = range || "24h";
    lastSohTrendRange = range;
    // 2026-08-11: 用户点击范围按钮(el!=null) → 防抖 + 中断旧请求 + 加载态(感知速度);
    //   定时刷新/初始化(el 为空)直接加载, 不打断用户查询也不弹加载态.
    if (el != null) {
        _chartSetLoading(sohTrendChart, true);
        if (_sohRangeTimer) clearTimeout(_sohRangeTimer);
        if (_sohAbort) { _sohAbort.abort(); _sohAbort = null; }
        _sohAbort = new AbortController();
        const _sig = _sohAbort.signal;
        _sohRangeTimer = setTimeout(() => {
            _sohRangeTimer = null;
            _loadSohTrendCore(range, el, _sig, true);
        }, 300);
        return;
    }
    _loadSohTrendCore(range, el, null, false);
}

// ============================================================
// v2: 电池健康度分析 —— 多因子评分 + 双模型衰减预测 + RUL
// 纯前端,基于 /api/history 历史数据;因子无数据则自适应剔除权重
// ============================================================

// 清洗脏点(soh<=0/NaN) + 分桶聚合(soh/temp/vdelta/cycle 各自逐桶序列)
function _sohCleanBuckets(rows, range) {
    const minMap = { "24h": 1440, "7d": 10080, "30d": 43200 };
    const bucketDays = (range === "24h") ? 1 / 24 : 1;
    const buckets = {};
    let spanDays = false;
    if (rows.length >= 2) {
        const d0 = new Date(rows[0].recv_time * 1000), d1 = new Date(rows[rows.length - 1].recv_time * 1000);
        spanDays = (d0.getFullYear() !== d1.getFullYear() || d0.getMonth() !== d1.getMonth() || d0.getDate() !== d1.getDate());
    }
    rows.forEach(row => {
        const soh = Number(row.soh || 0);
        if (!(soh > 0)) return;                       // 清洗脏点
        const d = new Date(row.recv_time * 1000);
        const mo = String(d.getMonth() + 1).padStart(2, "0");
        const da = String(d.getDate()).padStart(2, "0");
        let key;
        if (range === "24h") key = spanDays ? mo + "-" + da + " " + d.getHours() + ":00" : d.getHours() + ":00";
        else key = mo + "-" + da;
        if (!buckets[key]) buckets[key] = { soh: [], temp: [], vm: [], cyc: [] };
        const b = buckets[key];
        b.soh.push(soh);
        const tmax = Number(row.temp_max || 0);
        if (tmax > 0) b.temp.push(tmax / 10.0);        // 0.1℃ → ℃
        const vmax = Number(row.v_max || 0), vmin = Number(row.v_min || 0);
        if (vmax > 0 && vmin > 0) b.vm.push(vmax - vmin);  // 单体压差 mV
        const cyc = Number(row.cycle_count || 0);
        if (cyc > 0) b.cyc.push(cyc);
    });
    const keys = Object.keys(buckets);
    const labels = [], soh = [], temp = [], vdelta = [], cycle = [];
    keys.forEach(k => {
        const b = buckets[k];
        labels.push(k);
        soh.push(Number((b.soh.reduce((a, x) => a + x, 0) / b.soh.length).toFixed(2)));
        temp.push(b.temp.length ? Number((b.temp.reduce((a, x) => a + x, 0) / b.temp.length).toFixed(1)) : null);
        vdelta.push(b.vm.length ? Number((b.vm.reduce((a, x) => a + x, 0) / b.vm.length).toFixed(0)) : null);
        cycle.push(b.cyc.length ? Math.max.apply(null, b.cyc) : null);
    });
    return { labels, soh, temp, vdelta, cycle, bucketDays };
}

// 多因子健康评分(自适应加权: 无数据因子自动剔除并归一化)
function _sohHealthScore(sohArr, tempArr, vdeltaArr, cycleArr) {
    const lastSoh = sohArr[sohArr.length - 1] || 0;
    const factors = [];
    factors.push({ key: "capacity", name: "容量保持(SOH)", value: lastSoh.toFixed(0) + "%", score: Math.max(0, Math.min(100, lastSoh)), avail: true });
    const cyc = cycleArr.filter(x => x != null);
    if (cyc.length && Math.max.apply(null, cyc) > 0) {
        const rated = 2000;                                  // 额定循环次数(按电池规格配置)
        const ratio = Math.min(1, Math.max.apply(null, cyc) / rated);
        factors.push({ key: "cycle", name: "循环老化", value: Math.max.apply(null, cyc) + " 次", score: Math.round((1 - ratio) * 100), avail: true });
    } else {
        factors.push({ key: "cycle", name: "循环老化", value: "无数据", score: null, avail: false });
    }
    const tm = tempArr.filter(x => x != null);
    if (tm.length && Math.max.apply(null, tm) > 5) {
        const avgT = tm.reduce((a, x) => a + x, 0) / tm.length;
        const stress = Math.max(0, (avgT - 25) / (55 - 25));  // 25~55℃ 应力曲线
        factors.push({ key: "thermal", name: "温度应力", value: avgT.toFixed(1) + "℃", score: Math.round(Math.max(0, 1 - stress) * 100), avail: true });
    } else {
        factors.push({ key: "thermal", name: "温度应力", value: "数据不足", score: null, avail: false });
    }
    const vd = vdeltaArr.filter(x => x != null);
    if (vd.length && Math.max.apply(null, vd) > 0) {
        const avgV = vd.reduce((a, x) => a + x, 0) / vd.length;
        const tol = 200;                                    // 200mV 一致性容差
        factors.push({ key: "consist", name: "单体一致性", value: avgV.toFixed(0) + "mV", score: Math.round(Math.max(0, Math.min(1, 1 - avgV / tol)) * 100), avail: true });
    } else {
        factors.push({ key: "consist", name: "单体一致性", value: "数据不足", score: null, avail: false });
    }
    const W = { capacity: 0.6, cycle: 0.2, thermal: 0.1, consist: 0.1 };
    let tw = 0, sum = 0;
    factors.forEach(f => { if (f.avail) { tw += (W[f.key] || 0); sum += (W[f.key] || 0) * f.score; } });
    const score = tw > 0 ? Math.round(sum / tw) : Math.round(lastSoh);
    return { score, factors };
}

// 双模型拟合: 线性 + 指数, 按 R² 择优
function _sohFit(sohArr, bucketDays) {
    const n = sohArr.length;
    if (n < 3) return null;
    const X = []; for (let i = 0; i < n; i++) X.push(i * bucketDays);
    let sx = 0, sy = 0, sxy = 0, sxx = 0;
    for (let i = 0; i < n; i++) { sx += X[i]; sy += sohArr[i]; sxy += X[i] * sohArr[i]; sxx += X[i] * X[i]; }
    const denomL = n * sxx - sx * sx;
    const bL = denomL !== 0 ? (n * sxy - sx * sy) / denomL : 0;
    const aL = (sy - bL * sx) / n;
    const meanY = sy / n;
    let ssTot = 0, ssResL = 0;
    for (let i = 0; i < n; i++) { const yh = aL + bL * X[i]; ssTot += (sohArr[i] - meanY) * (sohArr[i] - meanY); ssResL += (sohArr[i] - yh) * (sohArr[i] - yh); }
    const r2L = ssTot > 0 ? 1 - ssResL / ssTot : 0;
    let ok = true; const lnY = [];
    for (let i = 0; i < n; i++) { if (sohArr[i] > 0) lnY.push(Math.log(sohArr[i])); else { ok = false; break; } }
    let bE = 0, aE = 0, r2E = 0;
    if (ok) {
        let sx2 = 0, sy2 = 0, sxy2 = 0, sxx2 = 0;
        for (let i = 0; i < n; i++) { sx2 += X[i]; sy2 += lnY[i]; sxy2 += X[i] * lnY[i]; sxx2 += X[i] * X[i]; }
        const denomE = n * sxx2 - sx2 * sx2;
        const bEraw = denomE !== 0 ? (n * sxy2 - sx2 * sy2) / denomE : 0;
        const aEraw = (sy2 - bEraw * sx2) / n;
        bE = bEraw; aE = aEraw;
        const meanLn = sy2 / n;
        let ssTotE = 0, ssResE = 0;
        for (let i = 0; i < n; i++) { const yh = aEraw + bEraw * X[i]; ssTotE += (lnY[i] - meanLn) * (lnY[i] - meanLn); ssResE += (lnY[i] - yh) * (lnY[i] - yh); }
        r2E = ssTotE > 0 ? 1 - ssResE / ssTotE : 0;
    }
    const useExp = ok && r2E > r2L;
    return {
        linear: { a: aL, b: bL, r2: r2L }, exp: { a: aE, b: bE, r2: r2E },
        chosen: useExp ? "exp" : "linear", r2: useExp ? r2E : r2L,
        bL: bL, aL: aL, bE: bE, aE: aE, lastX: X[n - 1], n: n
    };
}

// RUL 估算(到寿命终点阈值 eol)
function _sohRUL(fit, eol) {
    if (!fit) return { days: null, status: "数据不足", ok: false };
    let days = null, status = "", ok = false;
    if (fit.chosen === "exp") {
        const A = Math.exp(fit.aE);
        if (fit.bE < 0 && A > 0) {
            if (eol < A && eol > 0) { days = Math.log(eol / A) / fit.bE - fit.lastX; ok = true; status = "衰减通道"; }
            else if (eol >= A) status = "当前已高于阈值,未衰减";
        } else status = "未进入衰减通道(平稳/上升)";
    } else {
        if (fit.bL < 0) {
            if (eol < fit.aL) { days = (eol - fit.aL) / fit.bL - fit.lastX; ok = true; status = "衰减通道"; }
            else status = "当前已高于阈值,未衰减";
        } else status = "未进入衰减通道(平稳/上升)";
    }
    return { days, status, ok };
}

function _sohGradeV2(score) {
    if (score >= 90) return { label: "A 优秀", color: "#10b981", bg: "rgba(16,185,129,0.14)" };
    if (score >= 75) return { label: "B 良好", color: "#06b6d4", bg: "rgba(6,182,212,0.14)" };
    if (score >= 60) return { label: "C 一般", color: "#f59e0b", bg: "rgba(245,158,11,0.14)" };
    if (score >= 40) return { label: "D 偏差", color: "#fb923c", bg: "rgba(251,146,60,0.14)" };
    return { label: "E 需更换", color: "#ef4444", bg: "rgba(239,68,68,0.14)" };
}

function _sohRenderFactors(factors) {
    const host = $("sohFactors");
    if (!host) return;
    host.innerHTML = "";
    factors.forEach(f => {
        const row = document.createElement("div");
        row.className = "soh-v2-fac";
        const avail = f.avail && f.score != null;
        const pct = avail ? Math.max(2, Math.min(100, f.score)) : 4;
        const col = !avail ? "#64748b" : f.score >= 75 ? "#10b981" : f.score >= 50 ? "#f59e0b" : "#ef4444";
        const valTxt = avail ? f.value + " · " + f.score : f.value;
        row.innerHTML = '<span class="name">' + f.name + '</span>' +
            '<span class="bar"><i style="width:' + pct + '%;background:' + col + '"></i></span>' +
            '<span class="val">' + valTxt + '</span>';
        host.appendChild(row);
    });
}

function _loadSohTrendCore(range, el, signal, showLoading) {
    // Bug2 修复: el 为空 (初始化/定时刷新) 时,自动按 range 找到对应按钮并补 active
    let btnEl = el;
    if (!btnEl) {
        const host = document.getElementById("sohTrendChart");
        if (host) {
            const card = host.closest(".chart-card");
            if (card) {
                const cand = card.querySelectorAll(".chart-actions .chart-tab");
                cand.forEach(b => {
                    const oc = b.getAttribute("onclick") || "";
                    if (oc.indexOf("'" + range + "'") >= 0) btnEl = b;
                });
            }
        }
    }
    if (btnEl) {
        const container = btnEl.closest(".chart-actions");
        if (container) container.querySelectorAll(".chart-tab").forEach(b => b.classList.remove("active"));
        btnEl.classList.add("active");
    }
    const minMap = { "24h": 1440, "7d": 10080, "30d": 43200 };
    const minutes = minMap[range] || 1440;
    fetch("/api/history?minutes=" + minutes + "&limit=20000", signal ? { signal } : undefined)
        .then(r => r.json())
        .then(res => {
            if (!sohTrendChart) return;
            const rows = res.data || [];
            if (rows.length === 0) {
                if (showLoading) _chartSetLoading(sohTrendChart, false);
                addLog("[SOH] 无历史数据", "log-error");
                return;
            }
            const agg = _sohCleanBuckets(rows, range);
            const labels = agg.labels, sohArr = agg.soh, bucketDays = agg.bucketDays;
            if (sohArr.length === 0) {
                if (showLoading) _chartSetLoading(sohTrendChart, false);
                addLog("[SOH] 清洗后无有效 SOH 数据", "log-error");
                return;
            }
            // ---- 健康评分 ----
            const hs = _sohHealthScore(sohArr, agg.temp, agg.vdelta, agg.cycle);
            const grade = _sohGradeV2(hs.score);
            const scoreEl = $("sohHealthScore");
            if (scoreEl) scoreEl.textContent = hs.score;
            const gradeEl = $("sohHealthGradeV2");
            if (gradeEl) { gradeEl.textContent = grade.label; gradeEl.style.color = grade.color; gradeEl.style.background = grade.bg; }
            _sohRenderFactors(hs.factors);

            // ---- 模型拟合 + RUL ----
            const eolEl = $("thSohLowWarn");
            const eol = eolEl ? (parseFloat(eolEl.value) || 80) : 80;   // 复用设置页"健康度衰减预警"作为寿命终点
            const fit = _sohFit(sohArr, bucketDays);
            const rul = _sohRUL(fit, eol);
            const modelBadge = $("sohModelBadge");
            const rulEl = $("sohRUL");
            if (fit && modelBadge) {
                const mname = fit.chosen === "exp" ? "指数衰减" : "线性";
                const conf = fit.r2 >= 0.6 ? "高" : fit.r2 >= 0.3 ? "中" : "低";
                modelBadge.textContent = "预测模型: " + mname + " (R²=" + fit.r2.toFixed(2) + ", 置信" + conf + ")";
            }
            if (rulEl) {
                if (rul.ok && isFinite(rul.days) && rul.days > 0) {
                    const d = Math.round(rul.days);
                    const ed = new Date(Date.now() + d * 86400000);
                    const edStr = ed.getFullYear() + "-" + String(ed.getMonth() + 1).padStart(2, "0") + "-" + String(ed.getDate()).padStart(2, "0");
                    rulEl.innerHTML = "剩余寿命(RUL): 约 <b style='color:#f59e0b'>" + d + "</b> 天 · 预计 " + edStr + " 达 " + eol + "%";
                } else if (rul.status.indexOf("未进入衰减") >= 0) {
                    rulEl.innerHTML = "剩余寿命(RUL): <b style='color:#10b981'>未进入衰减通道</b> · " + rul.status;
                } else if (rul.status.indexOf("已高于阈值") >= 0) {
                    rulEl.innerHTML = "剩余寿命(RUL): <b>当前已高于 " + eol + "%</b> · 无需更换预测";
                } else {
                    rulEl.textContent = "剩余寿命(RUL): 数据不足,无法预测";
                }
            }

            // ---- 图表: 实测 + 外推 + 阈值线 ----
            const n = sohArr.length;
            let extra;
            if (rul.ok && isFinite(rul.days) && rul.days > 0) extra = Math.min(range === "24h" ? 180 : 730, Math.max(6, Math.ceil(rul.days / bucketDays)));
            else extra = (range === "24h" ? 48 : 180);
            const allLabels = labels.slice();
            for (let i = 0; i < extra; i++) allLabels.push("→" + (i + 1));
            const pred = new Array(n).fill(null);
            for (let i = n; i < n + extra; i++) {
                const X = i * bucketDays;
                let v;
                if (fit && fit.chosen === "exp") v = Math.exp(fit.aE) * Math.exp(fit.bE * X);
                else if (fit) v = fit.aL + fit.bL * X;
                else v = sohArr[n - 1];
                v = Math.max(0, Math.min(105, v));
                pred.push(Number(v.toFixed(2)));
            }
            const eolLine = new Array(n + extra).fill(Number(eol));
            sohTrendChart.data.labels = allLabels;
            sohTrendChart.data.datasets[0].data = sohArr.concat(new Array(extra).fill(null));
            sohTrendChart.data.datasets[1].data = pred;
            sohTrendChart.data.datasets[1].label = fit && fit.chosen === "exp" ? "指数外推(%)" : "线性外推(%)";
            sohTrendChart.data.datasets[2].data = eolLine;
            sohTrendChart.data.datasets[2].label = "寿命终点(" + eol + "%)";
            sohTrendChart.update("none");

            // ---- 文字分析 ----
            const analysisEl = $("sohAnalysis");
            if (analysisEl) {
                const curSoh = sohArr[n - 1] || 0;
                const periodLabel = range === "24h" ? "24小时" : range === "7d" ? "7天" : "30天";
                const slopePerDay = fit ? (fit.chosen === "exp" ? (Math.exp(fit.bE) - 1) * 100 : fit.bL) : 0;
                const monthly = slopePerDay * 30;
                analysisEl.textContent = "📊 [" + periodLabel + "] SOH 当前 " + curSoh.toFixed(1) + "% · 健康评分 " + hs.score +
                    " · 日衰减 " + (slopePerDay >= 0 ? "+" : "") + slopePerDay.toFixed(3) + "%/d · 月衰减 " + (monthly >= 0 ? "+" : "") + monthly.toFixed(2) + "%/月";
            }
            if (showLoading) _chartSetLoading(sohTrendChart, false);
            addLog("[SOH] 健康度分析完成, " + n + " 个有效点, 评分 " + hs.score, "log-success");
        })
        .catch(err => {
            // 主动 abort(快速切换)不撤加载态, 下一个请求会接管
            if (err && err.name === "AbortError") return;
            if (showLoading) _chartSetLoading(sohTrendChart, false);
            addLog("[SOH] 加载失败: " + err, "log-error");
        });
}

// ============================================================
// 用户菜单(角色显示)—— 2026-08-21 修复(#4 假角色切换):
//   原实现点击循环切换角色文本但无真实权限(视觉假象);
//   现改为显示后端 session 真实角色, 点击仅刷新角色信息.
// ============================================================
function toggleUserMenu() {
    const role = $("userRole");
    if (!role) return;
    // 真实角色由后端 session 决定, 前端不可自改 —— 仅刷新显示
    fetch("/api/me").then(r => r.json()).then(d => {
        if (d && d.ok) {
            const rl = { admin: "管理员", operator: "操作员", viewer: "只读用户" };
            role.textContent = rl[d.role] || d.role || "未知";
            window._IS_ADMIN = !!d.is_admin;
            window._LOGIN_USER = d.user || window._LOGIN_USER;
            window._ME_LOADED = true;   // 2026-08-22 修复(#3): 角色已确认, 门控生效
            addLog("[用户] 当前角色: " + (rl[d.role] || d.role), "log-info");
            applyRoleGating();   // 重新应用门控
            try { applyConfigGating(); } catch (e) {}
        }
    }).catch(() => {});
}

// ============================================================
// 2026-08-21 角色门控(#4): viewer 只读(禁用控制面板/命令下发),
//   operator 可控制但不可改配置, admin 全权限. 由 fetchMe/toggleUserMenu 调用.
// ============================================================
function applyRoleGating() {
    const role = (window._CUR_ROLE || "viewer");
    const isAdmin = (window._IS_ADMIN === true);
    const isViewer = (role === "viewer");
    // 只读用户: 禁用控制面板所有开关/按钮(充放电总开关/充放MOS/均衡/开始停止/原始命令)
    if (isViewer) {
        document.querySelectorAll(".control-grid .toggle, .cmd-icon-btn, #btnSendCmd, [onclick*='startCharging'], [onclick*='startDischarging'], [onclick*='stopCharging'], [onclick*='stopDischarging'], [onclick*='toggleSwitch'], [onclick*='sendCommand'], [onclick*='toggleBalance']").forEach(el => {
            if (el && !el.disabled) {
                el.disabled = true;
                el.style.opacity = "0.5";
                el.style.cursor = "not-allowed";
                el.title = "只读用户无控制权限";
            }
        });
    } else {
        // 恢复可操作(解除之前可能的禁用)
        document.querySelectorAll(".control-grid .toggle, .cmd-icon-btn, #btnSendCmd, [onclick*='startCharging'], [onclick*='startDischarging'], [onclick*='stopCharging'], [onclick*='stopDischarging'], [onclick*='toggleSwitch'], [onclick*='sendCommand'], [onclick*='toggleBalance']").forEach(el => {
            if (el) {
                el.disabled = false;
                el.style.opacity = "";
                el.style.cursor = "";
                el.title = "";
            }
        });
    }
}

// ============================================================
// 2026-08-21 用户管理(仅管理员): 列表 / 新增 / 改角色 / 重置密码 / 删除
//   配套后端 /api/users CRUD(role_required("admin")).
// ============================================================
const _ROLE_LABEL = { admin: "管理员", operator: "操作员", viewer: "只读用户" };
let _curUsers = [];

function loadUsers() {
    fetch("/api/users").then(r => r.json()).then(d => {
        if (!d.ok) { addLog("[用户] 加载失败: " + (d.msg || d.error || ""), "log-error"); return; }
        _curUsers = d.users || [];
        const tbody = $("userTableBody");
        if (!tbody) return;
        if (!_curUsers.length) {
            tbody.innerHTML = '<tr><td colspan="7" style="color:var(--text-muted);padding:20px;text-align:center">暂无用户</td></tr>';
            return;
        }
        const me = window._LOGIN_USER || "admin";
        tbody.innerHTML = _curUsers.map(u => {
            const isMe = (u.username === me);
            const last = u.last_login ? new Date(u.last_login * 1000).toLocaleString() : "--";
            const created = u.created_at ? new Date(u.created_at * 1000).toLocaleString() : "--";
            return '<tr>' +
                '<td>' + u.id + '</td>' +
                '<td><strong>' + u.username + '</strong>' + (isMe ? ' <span style="color:var(--accent-green);font-size:11px">(当前)</span>' : '') + '</td>' +
                '<td>' + (u.display_name || "--") + '</td>' +
                '<td>' +
                '  <select class="device-select" style="padding:3px 8px;font-size:12px" onchange="updateUserRole(\'' + u.username + '\', this.value)" ' + (isMe ? 'disabled' : '') + '>' +
                '    <option value="admin"' + (u.role === "admin" ? " selected" : "") + '>管理员</option>' +
                '    <option value="operator"' + (u.role === "operator" ? " selected" : "") + '>操作员</option>' +
                '    <option value="viewer"' + (u.role === "viewer" ? " selected" : "") + '>只读用户</option>' +
                '  </select>' +
                '</td>' +
                '<td>' + created + '</td>' +
                '<td>' + last + '</td>' +
                '<td>' +
                (isMe
                    ? '<span style="color:var(--text-muted);font-size:11px">当前账号</span>'
                    : '<button class="command-btn" style="padding:3px 8px;font-size:11px" onclick="resetUserPwd(\'' + u.username + '\')">🔑 重置密码</button> ' +
                      '<button class="command-btn" style="padding:3px 8px;font-size:11px" onclick="deleteUserConfirm(\'' + u.username + '\')">🗑 删除</button>') +
                '</td>' +
                '</tr>';
        }).join("");
    }).catch(err => addLog("[用户] 加载异常: " + err, "log-error"));
}

function openAddUserModal() {
    const m = $("addUserModal");
    if (!m) return;
    ["addUserUsername", "addUserPassword", "addUserDisplay"].forEach(id => { const e = $(id); if (e) e.value = ""; });
    const r = $("addUserRole"); if (r) r.value = "viewer";
    m.classList.add("show");
    const f = $("addUserUsername"); if (f) f.focus();
}
function closeAddUserModal() {
    const m = $("addUserModal");
    if (m) m.classList.remove("show");
}
function submitAddUser() {
    const username = $("addUserUsername") ? $("addUserUsername").value.trim() : "";
    const password = $("addUserPassword") ? $("addUserPassword").value : "";
    const role = $("addUserRole") ? $("addUserRole").value : "viewer";
    const display_name = $("addUserDisplay") ? $("addUserDisplay").value.trim() : "";
    if (!username || username.length < 3) { showToast("用户名至少 3 个字符", "warn"); return; }
    if (!password || password.length < 8) { showToast("密码至少 8 位", "warn"); return; }
    fetch("/api/users", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username, password, role, display_name }),
    }).then(r => r.json()).then(d => {
        if (d.ok) {
            showToast(d.msg || "用户已创建", "success");
            closeAddUserModal();
            loadUsers();
        } else {
            showToast(d.msg || d.error || "创建失败", "error");
        }
    }).catch(err => showToast("请求异常: " + err, "error"));
}

function updateUserRole(username, role) {
    if (!confirm("确定将 " + username + " 的角色改为 " + (_ROLE_LABEL[role] || role) + " 吗?")) {
        loadUsers();   // 恢复下拉
        return;
    }
    fetch("/api/users/" + encodeURIComponent(username), {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ role }),
    }).then(r => r.json()).then(d => {
        if (d.ok) { showToast(d.msg || "角色已更新", "success"); addLog("[用户] " + d.msg, "log-success"); }
        else { showToast(d.msg || d.error || "更新失败", "error"); }
        loadUsers();
    }).catch(err => { showToast("请求异常: " + err, "error"); loadUsers(); });
}

function resetUserPwd(username) {
    const pw = prompt("为 " + username + " 设置新密码(至少 8 位):");
    if (!pw) return;
    if (pw.length < 8) { showToast("密码至少 8 位", "warn"); return; }
    fetch("/api/users/" + encodeURIComponent(username), {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ password: pw }),
    }).then(r => r.json()).then(d => {
        if (d.ok) { showToast(d.msg || "密码已重置", "success"); addLog("[用户] " + d.msg, "log-success"); }
        else showToast(d.msg || d.error || "重置失败", "error");
    }).catch(err => showToast("请求异常: " + err, "error"));
}

function deleteUserConfirm(username) {
    if (!confirm("确定删除用户 " + username + " 吗? 该操作不可撤销!")) return;
    fetch("/api/users/" + encodeURIComponent(username), { method: "DELETE" })
        .then(r => r.json()).then(d => {
            if (d.ok) { showToast(d.msg || "已删除", "success"); addLog("[用户] " + d.msg, "log-success"); }
            else showToast(d.msg || d.error || "删除失败", "error");
            loadUsers();
        }).catch(err => showToast("请求异常: " + err, "error"));
}

// ============================================================
// 修改密码(2026-08-08 多用户账号体系配套)
// ============================================================
function openChangePwdModal() {
    const m = $("changePwdModal");
    if (!m) return;
    const userEl = $("changePwdUser");
    if (userEl) userEl.textContent = window._LOGIN_USER || ($("userRole") ? $("userRole").textContent : "admin");
    ["changePwdOld", "changePwdNew", "changePwdNew2"].forEach(id => { const e = $(id); if (e) e.value = ""; });
    m.classList.add("show");   // 2026-08-08 修复: 改用 .show 类(与 balanceModal/diagModal 一致, 原 display 方式因 CSS opacity:0 不可见)
    const first = $("changePwdOld");
    if (first) first.focus();
}
function closeChangePwdModal() {
    const m = $("changePwdModal");
    if (m) m.classList.remove("show");
}
function submitChangePwd() {
    const oldPw = $("changePwdOld") ? $("changePwdOld").value : "";
    const newPw = $("changePwdNew") ? $("changePwdNew").value : "";
    const newPw2 = $("changePwdNew2") ? $("changePwdNew2").value : "";
    if (!oldPw || !newPw) { showToast("请填写旧密码和新密码", "warn"); return; }
    if (newPw.length < 8) { showToast("新密码至少 8 位", "warn"); return; }
    if (newPw !== newPw2) { showToast("两次输入的新密码不一致", "warn"); return; }
    fetch("/api/change_password", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ old_password: oldPw, new_password: newPw }),
    })
        .then(r => r.json())
        .then(res => {
            if (res.ok) {
                showToast(res.msg || "密码已修改, 请重新登录", "success");
                addLog("[账号] 密码修改成功, 即将重新登录", "log-success");
                // 2026-08-08 修复: 改密成功后强制重新登录
                setTimeout(() => { window.location.href = "/login"; }, 1200);
            } else {
                showToast(res.msg || "修改失败", "error");
                addLog("[账号] 密码修改失败: " + (res.msg || ""), "log-error");
            }
        })
        .catch(err => {
            showToast("请求失败: " + err, "error");
            addLog("[账号] 密码修改异常: " + err, "log-error");
        });
}

// ============================================================
// 移动端布局
// ============================================================
// ============================================================
// 视图切换(对齐原型 switchView): 侧栏导航 -> 显示对应 .view
// ============================================================
const VIEW_TITLES = {
    overview: ["总览", "实时电池状态监控"],
    cells:    ["电芯监控", "单体电压 / 3D 视图 / 明细"],
    trends:   ["趋势曲线", "电压 / 电流 / 温度 / SOH"],
    control:  ["控制 / 指令", "命令下发与参数设置"],
    alarms:   ["告警中心", "故障与事件"],
    reports:  ["报表分析", "日 / 周 / 月报与 CSV"],
    settings: ["系统设置", "OTA / 规则 / 推送 / 审计"]
};
function switchView(v) {
    const _prev = document.querySelector(".view.active");
    const _changed = !_prev || _prev.dataset.view !== v;
    document.querySelectorAll(".view").forEach(s => s.classList.toggle("active", s.dataset.view === v));
    document.querySelectorAll("#nav a").forEach(a => { const on = a.dataset.view === v; a.classList.toggle("active", on); on ? a.setAttribute("aria-current","page") : a.removeAttribute("aria-current"); });
    document.querySelectorAll(".mobile-nav a").forEach(a => { const on = a.dataset.view === v; a.classList.toggle("active", on); on ? a.setAttribute("aria-current","page") : a.removeAttribute("aria-current"); });
    // 仅在实际「切换到不同视图」时把滚动容器重置到顶部:
    //   - 修复"系统设置一进去在底部"(不同视图切换→回顶)
    //   - 避免同视图内点击/重复点击导航也跳顶(用户反馈"点击按键自动回到顶部")
    if (_changed) {
        const _cs = $("contentScroll");
        if (_cs) _cs.scrollTop = 0;
    }
    const t = VIEW_TITLES[v];
    const pt = $("pageTitle");
    if (t && pt) pt.innerHTML = t[0] + "<small>" + t[1] + "</small>";
    // 关闭移动端菜单
    const sb = $("sidebar"); if (sb) sb.classList.remove("open");
    const sc = $("scrim"); if (sc) sc.classList.remove("show");
    // 视图激活后再重绘图表(隐藏视图内 canvas 尺寸为 0, 需 resize 才能正确显示)
    requestAnimationFrame(() => {
        try {
            if (v === "overview") {
                if (mainChart) { mainChart.resize(); mainChart.update(); }
            } else if (v === "cells") {
                if (cellVoltChart) { cellVoltChart.resize(); cellVoltChart.update(); }
                if (typeof ensureBattery3DDOM === "function") ensureBattery3DDOM();
                if (window.B3D) { B3D.renderPack(); }
                // 2026-08-11: 切到电芯视图时立即强制重绘表格/3D/移动端(避免被实时节流延迟首帧)
                if (typeof renderCellTable === "function") renderCellTable(lastPayload ? lastPayload.cells : []);
                if (typeof updateMobileViz === "function") updateMobileViz();
                _last3DSig = (lastPayload && lastPayload.cells) ? lastPayload.cells.join(",") : " ";
                _lastCellTableSig = _last3DSig;
            } else if (v === "trends") {
                [historyChart, sohTrendChart, trendChart].forEach(c => { if (c) { c.resize(); c.update(); } });
            } else if (v === "reports") {
                if (reportChart) { reportChart.resize(); reportChart.update(); }
            } else if (v === "settings") {
                try { refreshRulesPushStatus(); } catch (e) { console.warn("refreshRulesPushStatus", e); }
                try { loadRulesConfig(); } catch (e) { console.warn("loadRulesConfig", e); }
                try { loadPushConfig(); } catch (e) { console.warn("loadPushConfig", e); }
                try { loadAuditLog(); } catch (e) { console.warn("loadAuditLog", e); }
                // 2026-08-22 修复(#3): 进入设置页时同步应用控件门控(admin 恢复 checkbox),
                //   不依赖 /api/me 异步时序 —— 解决"点击 checkbox 无响应"(残留 disabled)
                try { applyConfigGating(); } catch (e) { console.warn("applyConfigGating", e); }
            } else if (v === "alarms") {
                try { loadFaultHistory(); } catch (e) { console.warn("loadFaultHistory", e); }
            }
        } catch (e) { console.warn("switchView redraw:", e); }
    });
}

function toggleMobileMenu() {
    const sb = $("sidebar");
    if (!sb) return;
    const open = sb.classList.toggle("open");
    const sc = $("scrim");
    if (sc) sc.classList.toggle("show", open);
}

function mobNavSwitch(idx, el) {
    document.querySelectorAll(".mob-nav-item").forEach(b => b.classList.remove("active"));
    if (el) el.classList.add("active");
    const scroll = $("contentScroll");
    if (!scroll) return;
    const targets = [
        0,                                          // 总览
        document.querySelector(".cell-section"),    // 单体数据
        document.querySelector(".chart-section"),   // 历史
        $("alertCard"),                             // 告警
        document.querySelector(".control-card"),    // 设备管理
    ];
    const target = targets[idx];
    if (target && target.offsetTop) {
        scroll.scrollTo({ top: target.offsetTop - 80, behavior: "smooth" });
    } else {
        scroll.scrollTo({ top: 0, behavior: "smooth" });
    }
    toggleMobileMenu();
}

function updateMobileViz() {
    const el = $("mobileBatteryViz");
    if (!el) return;
    let html = "";
    for (let i = 0; i < NUM_CELLS; i++) {
        const v = cellData[i].voltage;
        // 配色: 过压红 / 欠压黄 / 正常绿 (修复: 原逻辑 v>3.5 显示黄, 与电压高低语义相反)
        const color = cellData[i].status === "error" ? "var(--accent-red)"
                    : cellData[i].status === "warn" ? "var(--accent-yellow)"
                    : (v > 4.2 || (v > 0 && v < 2.5)) ? "var(--accent-yellow)"
                    : "var(--accent-green)";
        html += '<div class="battery-cell" style="background:' + color + '" title="#' + (i + 1) + ": " + v.toFixed(3) + 'V">' +
                '<div class="cell-tip">#' + (i + 1) + ": " + v.toFixed(3) + "V</div>" +
                "</div>";
    }
    el.innerHTML = html;
}

function updateResponsive() {
    const w = window.innerWidth;
    const mobViz = $("mobileViz");
    if (mobViz) mobViz.style.display = w < 768 ? "block" : "none";
}

// ============================================================
// 参数加载(从 /api/params) -> 填充全部阈值输入框 + 保存到 window._BMS_PARAMS (供 parseFault 动态阈值)
// ============================================================
window._BMS_PARAMS = {};   // 全局: key -> {value, unit, default, ...}
// 2026-08-09: 正在编辑的输入框不回填(避免 30s 轮询/刷新打断用户参数设置)
function _skipFill(el) {
    return el && document.activeElement === el;
}
function loadParams() {
    fetch("/api/params")
        .then(r => r.json())
        .then(params => {
            if (!params || typeof params !== "object") return;
            window._BMS_PARAMS = params;
            // 2026-08-10 D3: 参数回比(设备确认生效检查)
            try { _checkParamVerify(params); } catch (e) { console.warn("param verify fail:", e); }
            // 串数动态适配: 参数面板里的 cell_series_num
            // 2026-08-09: 用户正在改串数时跳过(applySeriesNum 会重置 batSeries 输入框)
            // 2026-08-09 修复: value 为 0/非法时兜底用 default(6)——原条件 `params.cell_series_num.value`
            //   (0 为 falsy)不成立时直接跳过回填, 且若后端返回 0 会被 applySeriesNum 拦截导致
            //   batSeries/顶部串数显示异常(刷新后串数变 0). 改为显式兜底确保有效值回填.
            if (params.cell_series_num && !_skipFill($("batSeries"))) {
                let sv = Number(params.cell_series_num.value || 0);
                if (!(sv >= 1 && sv <= 16)) {
                    sv = Number(params.cell_series_num.default) || 16;   // 0/非法 → 兜底默认(BQ76952 16S)
                }
                // 2026-08-21 修复(#2 顶部串数刷新被改回): 用户通过网页配置过串数
                //   (GET /api/params source=user)时, 刷新后恢复 _userSeriesOverride,
                //   防止 handleBmsData 每帧用设备上报旧值(如 6)覆盖顶部显示。
                //   设备确认切换(上报 == 用户配置)后 handleBmsData 自动清除 override。
                if (params.cell_series_num.source === "user" && sv >= 1 && sv <= 32) {
                    _userSeriesOverride = sv;
                }
                applySeriesNum(sv);
            }
            // ===== 2026-08-08: 电池类型 / SOC 算法回填 + 旁显设备当前值 =====
            try {
                const TYPE_NAMES = { 0: "磷酸铁锂 (LFP)", 1: "三元锂 (NCM)", 2: "钛酸锂 (LTO)", 3: "铅酸" };
                // 2026-08-10 D4 修复: 补齐 3/4 两档(MCC-EKF/UKF)并与下拉选项文本完全一致,
                //   原 ALGO_NAMES 仅 0/1/2, 设备上报 soc_algo=3/4 时 select 无法回显(显示 undefined);
                //   索引 0 名称统一为 "AEKF双卡尔曼"(与发送标签 algoIdx 一致, 原为 "安时积分+卡尔曼")
                const ALGO_NAMES = { 0: "AEKF双卡尔曼", 1: "纯安时积分", 2: "开路电压法", 3: "MCC-EKF(默认最优)", 4: "UKF无迹卡尔曼" };
                if (params.battery_type && params.battery_type.value !== undefined) {
                    const bt = Number(params.battery_type.value);
                    const btEl = $("batType");
                    if (btEl && TYPE_NAMES[bt]) { btEl.value = TYPE_NAMES[bt]; }
                    const btTag = btEl && btEl.parentElement ? btEl.parentElement.querySelector(".cur-dev-val") : null;
                    if (btTag) btTag.textContent = "设备当前: " + (TYPE_NAMES[bt] || bt);
                }
                if (params.soc_algo && params.soc_algo.value !== undefined) {
                    const sa = Number(params.soc_algo.value);
                    const saEl = $("socAlgo");
                    if (saEl && ALGO_NAMES[sa]) { saEl.value = ALGO_NAMES[sa]; }
                    const saTag = saEl && saEl.parentElement ? saEl.parentElement.querySelector(".cur-dev-val") : null;
                    if (saTag) saTag.textContent = "设备当前: " + (ALGO_NAMES[sa] || sa);
                }
                if (params.cell_capacity_mah && params.cell_capacity_mah.value !== undefined) {
                    const capEl = $("batCapacity");
                    if (capEl) capEl.value = (Number(params.cell_capacity_mah.value) / 1000).toFixed(1);
                }
            } catch (e) { console.warn("电池参数回填失败:", e); }
            // 将后端参数填充到阈值输入框(通用 THRESHOLD_MAP 映射)
            try {
                let filled = 0;
                THRESHOLD_MAP.forEach(([id, key, mul]) => {
                    const el = $(id);
                    const meta = params[key];
                    if (!el || !meta) return;
                    // 2026-08-09: 正在编辑的输入框跳过回填(不打断用户输入), 旁显"设备当前"仍更新
                    if (_skipFill(el)) {
                        let curTag2 = el.parentElement ? el.parentElement.querySelector(".cur-dev-val") : null;
                        if (curTag2) curTag2.textContent = "设备当前: " + el.value;
                        return;
                    }
                    let v = meta.value / mul;
                    if (mul === 1000) el.value = Number(v.toFixed(3));
                    else if (mul === 10) el.value = Number(v.toFixed(1));
                    else el.value = Number(v.toFixed(2));
                    // 2026-08-08: 在输入框旁标注"设备当前值", 便于下发前后对照
                    const unit = meta.unit === "mV" ? "V" : meta.unit === "dc" ? "℃" : (meta.unit === "ma" ? "A" : meta.unit);
                    let curTag = el.parentElement ? el.parentElement.querySelector(".cur-dev-val") : null;
                    if (!curTag && el.parentElement) {
                        curTag = document.createElement("span");
                        curTag.className = "cur-dev-val";
                        curTag.style.cssText = "display:block;font-size:10px;color:var(--accent-cyan);margin-top:2px";
                        el.parentElement.appendChild(curTag);
                    }
                    if (curTag) {
                        curTag.textContent = "设备当前: " + el.value + (unit || "");
                    }
                    filled++;
                });
                addLog("[参数] 已加载 " + Object.keys(params).length + " 个参数 · 填充 " + filled + " 项到表单", "log-info");
            } catch (e) {
                console.warn("参数填充失败:", e);
            }
        })
        .catch(err => {
            addLog("[参数] 加载失败: " + err, "log-error");
        });
}

// ============================================================
// 数据处理: 接收 bms_data 事件
// ============================================================
// ============================================================
// 传感器未接线提示: 设备在线但 BQ76952 未接(电压/温度/SOC 全 0)时
// 显示醒目横幅, 避免用户误以为"设备没传数据"
// ============================================================
// 传感器未接线横幅是否被用户手动关闭(会话级, 数据恢复正常后自动重置)
let _sensorHintDismissed = false;

function dismissSensorHint() {
    _sensorHintDismissed = true;
    const el = document.getElementById("sensorHintBanner");
    if (el) el.remove();
}

function setSensorHint(show, devOnline) {
    let el = document.getElementById("sensorHintBanner");
    if (show) {
        // 用户已手动关闭 → 本次会话不再打扰(数据正常后会自动重置)
        if (_sensorHintDismissed) {
            if (el) el.remove();
            return;
        }
        if (!el) {
            el = document.createElement("div");
            el.id = "sensorHintBanner";
            Object.assign(el.style, {
                position: "fixed", top: "34px", left: "0", right: "0", zIndex: "9998",
                background: "#7a5c00", color: "#fff",
                textAlign: "center", padding: "5px 34px 5px 12px", fontSize: "12.5px",
                fontWeight: "600", boxShadow: "0 2px 6px rgba(0,0,0,0.25)",
                lineHeight: "1.45"
            });
            el.innerHTML = '<span style="display:inline-block">⚠ 设备在线，但 BQ76952 电压采样未接线：电压/SOC/温度显示为 0（电流正常）。接线后数据自动恢复。</span>' +
                '<button id="sensorHintClose" title="关闭提示" style="position:absolute;right:8px;top:50%;transform:translateY(-50%);' +
                'background:rgba(255,255,255,0.15);border:none;color:#fff;width:22px;height:22px;border-radius:4px;' +
                'cursor:pointer;font-size:14px;line-height:1;display:flex;align-items:center;justify-content:center;' +
                'padding:0">×</button>';
            document.body.appendChild(el);
            $("sensorHintClose").addEventListener("click", function (e) {
                e.stopPropagation();
                dismissSensorHint();
            });
        }
    } else {
        // 数据已恢复正常: 重置关闭状态, 下次异常继续提示
        _sensorHintDismissed = false;
        if (el) el.remove();
    }
}

// ===== 2026-08-11 实时渲染节流(解决卡顿/按键响应慢/出图慢) =====
// 实时消息(handleBmsData)约每 1~2s 触发一次, 原逻辑每次都全量重绘 3D 透视+光照/整张表格/
// 整份告警列表/移动端可视化 + 图表 update, 主线程长期被占满 -> 点击/输入卡顿.
// 按"元素可见 + 最小间隔 + 数据变化"三重门控, 把重渲染降到必要最低; 隐藏视图的图表跳过
// update(切回时 switchView 已做整图重绘, 数据不丢).
var _rtGate = {};
function _gateRun(key, minMs, fn) {
    var now = (typeof performance !== "undefined") ? performance.now() : Date.now();
    if (now - (_rtGate[key] || 0) < minMs) return;
    _rtGate[key] = now;
    try { fn(); } catch (e) { console.warn("rtGate:" + key, e); }
}
function _elVisible(id) {
    var e = document.getElementById(id);
    if (!e) return false;
    if (e.offsetParent !== null) return true;       // 正常文档流中可见
    var r = e.getBoundingClientRect();
    return (r.width > 0 && r.height > 0);           // position:fixed 等兜底
}
var _last3DSig = " ";
var _lastCellTableSig = " ";
var _lastAlertFault = undefined;
// 告警列表仅在故障码变化时整表重建(避免每帧重建 innerHTML)
function renderAlertListIfChanged(faultCode) {
    if (faultCode !== _lastAlertFault) { _lastAlertFault = faultCode; renderAlertList(faultCode); }
}

function handleBmsData(input) {
    if (!input) return;
    // ---------- 归一化: 兼容 3 种来源 ----------
    //  1) SocketIO bms_data: 直接就是 payload
    //  2) /api/status (平铺): s.pack_v / s.soc 在顶层, 且 s.data 也存在
    //  3) /api/status (旧格式): s = {ok,data:{...}}
    let p = input;
    if (typeof p === "object" && p !== null && p.data && typeof p.data === "object"
        && (typeof p.ok !== "undefined" || typeof p.device_online !== "undefined" || typeof p.db_count !== "undefined")) {
        /* s = /api/status 响应(双格式) => 优先用顶层平铺(p);
         * Bug1 修复: 顶层若 pack_v==0 且 soc==0 且 cells 全 0 (空壳上报),
         * 而 p.data 存在有效数据 (cells/pack_v>0) 则应降级用 p.data,
         * 否则 KPI 会一直显示 "0-" 而历史曲线有数据 (历史来自 DB query_latest 跳过空壳) */
        const tPack = Number(p.pack_v || 0);
        const tSoc  = Number(p.soc || 0);
        const tCells = p.cells && Array.isArray(p.cells) ? p.cells : [0,0,0,0,0,0];
        const topEmpty = (tPack <= 0 && tSoc <= 0.01 && tCells.every(v => Number(v||0) === 0));
        const dPack = Number((p.data.pack_v) || 0);
        const dSoc  = Number((p.data.soc) || 0);
        const dCells = p.data.cells && Array.isArray(p.data.cells) ? p.data.cells : [0,0,0,0,0,0];
        const dataValid = (dPack > 0 || dSoc > 0.01 || dCells.some(v => Number(v||0) > 0));
        if (topEmpty && dataValid) {
            p = p.data;   /* 顶层空壳 + data 有有效 => 用 data */
        }
    }
    if (!p) return;
    // ===== 串数动态适配 =====
    // 2026-08-08 修复: 优先用后端配置串数(cell_series_num, 保存时已同步全局),
    //   避免设备上报 cells 全 0(BQ76952 未接)/旧固件只报 6 格时把前端拉回 6 串,
    //   导致用户切到 16 串后无法查询 >6 串数据.
    const _cellsArr = (p.cells && Array.isArray(p.cells)) ? p.cells : null;
    const _csn = Number(p.cell_series_num || 0);
    // 2026-08-09 修复: 用户下发串数后 _userSeriesOverride>0——
    //   handleBmsData 每帧收到后端 cell_series_num(=设备上报 16)会 applySeriesNum 覆盖
    //   用户配置("适配一秒又回16"). override 生效期间: 保持用户配置, 不用设备值覆盖;
    //   设备确认切换(上报 == 用户配置)后清除 override, 恢复由设备值驱动.
    if (_userSeriesOverride >= 1 && _userSeriesOverride <= 32) {
        if (_csn === _userSeriesOverride) {
            _userSeriesOverride = 0;      // 设备确认切换成功, 解除锁定
        } else {
            applySeriesNum(_userSeriesOverride);   // 保持用户配置(不覆盖)
        }
    } else if (_csn >= 1 && _csn <= 32) {   // 上限 16→32(与固件 1~32S 一致)
        applySeriesNum(_csn);   // 优先: 后端配置串数(用户下发/设备参数)
    } else if (_cellsArr && _cellsArr.length > 0 && _cellsArr.some(v => Number(v || 0) > 0)) {
        applySeriesNum(_cellsArr.length);   // 兜底: 设备真实采集到有效数据时以长度为准
    }
    // ===== 问题1/4修复: 延迟/新鲜度计算 + 自动单位 =====
    // 1) 优先用后端已算好的 data_age_ms (避免前端乱乘1000)
    // 2) 否则用 last_event_time_ms (有效Unix ms)
    // 3) 对无效 timestamp (如 ESP32 boot tick 364780) 前端也二次过滤 (<2000年视为无效)
    const MIN_UNIX_MS = 946684800 * 1000; // 2000-01-01
    let ageMs = 0;
    if (p.no_data) {
        ageMs = 0;
    } else if (typeof p.data_age_ms === "number" && p.data_age_ms > 0) {
        ageMs = p.data_age_ms;
    } else if (typeof p.last_event_time_ms === "number" && p.last_event_time_ms > MIN_UNIX_MS) {
        ageMs = Math.max(0, Date.now() - p.last_event_time_ms);
    } else if (p.timestamp) {
        let ts = Number(p.timestamp || 0);
        if (ts < 1e12) ts = ts * 1000;
        if (ts > MIN_UNIX_MS) {
            ageMs = Math.max(0, Date.now() - ts);
        } else {
            ageMs = 0; /* 无效 boot tick, 跳过 */
        }
    }
    // 2026-08-19 断网优化: 合并 EMQX 直连新鲜度(与 updateCommStatus 一致)。
    //   原逻辑仅用华为云影子 data_age_ms(设备详情 API 延迟 1~2min), 设备断电后
    //   影子仍报 ONLINE 期间前端主路径误判"在线"; EMQX 消费腿断电后立即无新帧,
    //   emqx_age_ms 迅速增大, 用它兜底保证 ~60s 内转离线, 不依赖影子延迟.
    if (typeof p.emqx_age_ms === "number" && p.emqx_age_ms >= 0) {
        if (ageMs <= 0 || p.emqx_age_ms > ageMs) {
            ageMs = p.emqx_age_ms;
        }
    }
    latencyMs = ageMs;
    // 自动单位显示: ms (<1s) / s (<1min) / min (<1h) / h, 并标注"实时/缓存"
    if ($("latency")) {
        if (p.no_data || ageMs <= 0) {
            $("latency").textContent = "--";
        } else {
            let txt = "";
            if (ageMs < 1000) txt = ageMs + "ms";
            else if (ageMs < 60 * 1000) txt = (ageMs/1000).toFixed(1) + "s";
            else if (ageMs < 3600 * 1000) txt = (ageMs/60000).toFixed(1) + "min";
            else txt = (ageMs/3600000).toFixed(1) + "h";
            $("latency").textContent = txt;
        }
    }
    // ---------- 可视化去重(避免 HTTP 轮询 + SocketIO 同一条数据重复 push 到曲线) ----------
    // 修复: 原用 p.timestamp(ESP32 boot tick, 每次都变)导致去重失效
    //       改用 last_event_time_ms(华为云影子事件时间, 同一帧数据时间相同)
    // 2026-08-10 修复: 影子事件时间缺失(设备离线/影子过期时后端返回 None)时,
    //   原 key 恒为 "-|0|0" → sameAsLast 恒 true → pushMainChart/pushCellVoltChart
    //   永不执行 → "实时数据有但曲线全没". 时间分量加两级兜底:
    //   last_event_time_ms → 设备 timestamp → 本地时间, 保证不同帧 key 必不同,
    //   同时同帧(SocketIO+轮询双通道)仍能去重.
    const _evtMs = Number(p.last_event_time_ms || 0);
    const _tsMs  = Number(p.timestamp || 0);
    const _timeComp = (_evtMs > 946684800000) ? _evtMs
                    : (_tsMs > 946684800000) ? _tsMs
                    : Date.now();
    const key = (p.id || "-") + "|" + _timeComp + "|" + (p.current || "0");
    const sameAsLast = (key === _lastVisualKey);
    // 但: 即使相同也仍然更新 KPI 卡片(刷新延迟显示/数据源标识), 不更新实时曲线
    if (!sameAsLast) {
        _lastVisualKey = key;
    }
    // ===== 严格二值在线判定(用户明确要求: 只分在线/离线; 不要疑似失联中间态) =====
    // 在线 = 两个条件同时满足:
    //   A. API明确说在线(device_online=True / device_status=ONLINE / data_realtime=True)
    //   B. 且 data_age ≤ OFFLINE_MS (3.3min 还没新数据 = 失联肯定发生了, 强判离线不商量)
    const OFFLINE_MS = 10 * 1000;   // 2026-08-21: 20s→10s(用户要求 10s 内判定离线)
    let apiOnline = (typeof p.device_online === "boolean") ? p.device_online
                  : (p.device_status === "ONLINE");
    if (p.data_realtime && !apiOnline) apiOnline = true; /* 影子新鲜=肯定在线 */
    // 2026-08-21 修复(#2 防闪根因): socketio 实时帧(EMQX 直连 bms_data)来自
    //   props_to_payload, 不含 device_online/device_status/data_realtime 字段,
    //   若仅凭这三字段判定, 每 7s 全量帧到达都会误判离线(updateDeviceOnlineUI(false)),
    //   而 15s 轮询(/api/status 含 device_online=true)又拉回在线 → 循环闪。
    //   与 no_data 分支一致: 帧新鲜(ageMs<=OFFLINE_MS)即证明设备在线, 权威优先。
    if (!apiOnline && !p.no_data && ageMs > 0 && ageMs <= OFFLINE_MS) {
        apiOnline = true;
    }
    const ageOffline = (ageMs > 0) && (ageMs > OFFLINE_MS);
    let realOnline = apiOnline && !ageOffline; /* 任何一条不满足 = 离线 */
    // 历史缓存徽章保留: 不在线 / 非实时 / no_data 以外 => 显示橙色缓存标识(只标注,不影响状态二值)
    const isCached = !p.no_data && (!p.data_realtime || !realOnline);
    window.__devAgeMs = ageMs;

    // 无数据(ESP32 离线/传感器未接): KPI 显示"--"、曲线断线、表格空
    if (p.no_data) {
        lastPayload = p;
        // 从后端返回的 last_pack_v 初始化灰色虚线的起始值(断线指示线)
        if (p.last_pack_v && p.last_pack_v > 0 && lastValidVoltV <= 0) {
            lastValidVoltV = p.last_pack_v / 1000;
        }
        // 2026-08-19 修复: no_data 分支也按 EMQX 新鲜度判在线。原逻辑直接用华为云影子
        //   p.device_online(设备详情 API 延迟 1~2min, 断电后仍返回 ONLINE), 导致"设备掉线
        //   网页仍显示在线很久"。与 updateCommStatus 一致: emqx_age_ms > 60s 即离线。
        const _offlineMs = 10 * 1000;
        let _ageMs = Number(p.data_age_ms || 0);
        if (typeof p.emqx_age_ms === "number" && p.emqx_age_ms >= 0) {
            _ageMs = Math.max(_ageMs, p.emqx_age_ms);
        }
        const _emqxAlive = (typeof p.emqx_age_ms === "number" && p.emqx_age_ms >= 0 && p.emqx_age_ms <= _offlineMs);
        const _apiOnline = (typeof p.device_online === "boolean") ? !!p.device_online : (p.device_status === "ONLINE");
        // 2026-08-20 修复(#2 防闪): 与 updateCommStatus 一致 —— EMQX 直连新鲜=在线,
        //   权威优先。原写法 `&& !(_ageMs > _offlineMs)` 会被华为云影子 age 拖累:
        //   设备在线时华为云 API 延迟导致影子 age>60s, 一秒判在线一秒判离线循环闪。
        const isDevOnline = _emqxAlive || (_apiOnline && !(_ageMs > _offlineMs));
        updateDeviceOnlineUI(isDevOnline, p.device_status || (isDevOnline ? "ONLINE" : "OFFLINE"));
        updateDataSource(true, p.data_source, isDevOnline ? "设备在线(数据待同步)" : "设备离线");
        _applyKpiCacheBadge(false, 0);
        _gateRun("kpi", 200, function () { updateKPI(p); });   // 节流: KPI 最快 200ms/次
        if (_elVisible("battery3D")) {
            var _s3 = (p.cells || []).join(",");
            if (_s3 !== _last3DSig) { _last3DSig = _s3; _gateRun("3d", 200, function () { renderBattery3D(p.cells); }); }
        }
        if (_elVisible("cellTableBody")) {
            var _s4 = (p.cells || []).join(",");
            if (_s4 !== _lastCellTableSig) { _lastCellTableSig = _s4; _gateRun("celltable", 300, function () { renderCellTable(p.cells); }); }
        }
        if (!sameAsLast) {
            if (_elVisible("mainChart")) pushMainChart(p);           // no_data 时 push null 形成断线(仅可见图)
            if (_elVisible("cellVoltChart")) pushCellVoltChart(p);   // 16路单体电压曲线同步(仅可见图)
        }
        renderAlertListIfChanged(0);
        _gateRun("mobviz", 500, updateMobileViz);
        return;
    }
    lastPayload = p;
    // ===== 关键: 不再强行 ONLINE! 用真实判定 =====
    if (typeof p.device_status === "string" && p.device_status) {
        updateDeviceOnlineUI(realOnline, p.device_status);
    } else if (realOnline) {
        updateDeviceOnlineUI(true, "ONLINE");
    } else {
        // 2026-08-19 修复(C2): 离线且该帧无 device_status 时也立即更新状态点——
        //   原实现此分支不调用 updateDeviceOnlineUI, 顶栏最多延迟 15s(等轮询)才转离线
        updateDeviceOnlineUI(false, "OFFLINE");
    }
    // 数据来源标识(细粒度)
    const srcHint = isCached ? "历史缓存数据" : null;
    updateDataSource(!!p.is_mock, p.data_source, srcHint);
    // ===== 传感器未接线提示: 设备在线但 BQ76952 未接(电压/温度/SOC 全 0) =====
    //   仅 current 有值(电流 ADC 独立于 BQ76952), 数据链路本身正常
    const noVolt = (!p.pack_v || p.pack_v <= 0)
        && (!p.v_max || p.v_max <= 0)
        && (!(p.cells && p.cells.some && p.cells.some(v => Number(v || 0) > 0)));
    const noTemp = (!p.temp_max || p.temp_max <= 0) && (!p.temp_min || p.temp_min <= 0);
    const noSoc  = (!p.soc || p.soc <= 0.01);
    setSensorHint(realOnline && !p.no_data && noVolt && noTemp && noSoc, realOnline);
    // KPI 缓存徽章 (设备离线 + 有 DB 历史数据 = 明显提示)
    _applyKpiCacheBadge(isCached, ageMs);
    // KPI / 单体表格
    _gateRun("kpi", 200, function () { updateKPI(p); });   // 节流: KPI 最快 200ms/次
    if (_elVisible("battery3D")) {
        var _s1 = (p.cells || []).join(",");
        if (_s1 !== _last3DSig) { _last3DSig = _s1; _gateRun("3d", 200, function () { renderBattery3D(p.cells); }); }
    }
    if (_elVisible("cellTableBody")) {
        var _s2 = (p.cells || []).join(",");
        if (_s2 !== _lastCellTableSig) { _lastCellTableSig = _s2; _gateRun("celltable", 300, function () { renderCellTable(p.cells); }); }
    }
    // ===== 2026-08-08 修复: 设备回读 MOS 状态同步前端开关(数据统一性) =====
    //   设备保护动作/其他端操作会改变 charge_mos/discharge_mos,
    //   前端 toggle 开关需跟随设备真实状态, 避免界面与设备脱节
    try {
        const chgM = Number(p.charge_mos ?? -1);
        const dsgM = Number(p.discharge_mos ?? -1);
        if (chgM === 0 || chgM === 1) {
            const t = document.querySelectorAll(".control-grid .toggle")[1];
            if (t) t.classList.toggle("active", chgM === 1);
            switchState.ChgMOS = (chgM === 1);
        }
        if (dsgM === 0 || dsgM === 1) {
            const t = document.querySelectorAll(".control-grid .toggle")[2];
            if (t) t.classList.toggle("active", dsgM === 1);
            switchState.DischgMOS = (dsgM === 1);
        }
        // 充放电总开关 = 充放任一开启
        const mainOn = (switchState.ChgMOS || switchState.DischgMOS);
        const mainT = document.querySelectorAll(".control-grid .toggle")[0];
        if (mainT) mainT.classList.toggle("active", mainOn);
        switchState.MainRelay = mainOn;
        // 均衡状态
        const bal = Number(p.balance_on ?? (Number(p.balance_mask ?? 0) !== 0 ? 1 : 0));
        const balT = document.querySelectorAll(".control-grid .toggle")[3];
        if (balT) balT.classList.toggle("active", bal === 1);
        switchState.Balance = (bal === 1);
    } catch (e) {}
    // 实时曲线 (仅当数据更新时, 防止 5s 轮询重复叠加)
    if (!sameAsLast) {
        if (_elVisible("mainChart")) pushMainChart(p);           // 仅可见图实时刷新(隐藏图跳过 update, 切回时 switchView 整图重绘)
        if (_elVisible("cellVoltChart")) pushCellVoltChart(p);   // 16路单体电压曲线同步(仅可见图)
    }
    // 2026-08-11 修复"反应慢": 移除实时消息触发的整段历史重拉——设备持续上报时,
    //   原本每 ~2s 就重新拉取全部历史(7d 高达 1 万行)+ 重新聚合 + 重绘, 卡顿明显.
    //   历史趋势图改为仅依赖下方 60s 定时器刷新(对历史视图足够实时), 大幅降低主线程占用.
    // 告警列表(仅故障码变化才整表重建)
    renderAlertListIfChanged(p.fault);
    // 移动端可视化(节流)
    _gateRun("mobviz", 500, updateMobileViz);
    // 故障时记录日志
    if (p.fault && Number(p.fault) !== 0) {
        const alerts = parseFault(p.fault);
        alerts.forEach(a => {
            addOperationLog("故障: " + a.desc + " (0x" + a.code.toString(16).toUpperCase() + ")", "error");
        });
    }
}

// ============================================================
// Socket.IO 事件绑定
// ============================================================
function initSocket() {
    if (!window.io) {
        addLog("Socket.IO 未加载", "log-error");
        return;
    }
    // ===== 2026-08-09 高可靠加固: SocketIO 显式自动重连配置 =====
    //   reconnection 默认开启, 显式配置次数/延迟/抖动, 保证 7×24 断网恢复后自动回连;
    //   transports 优先 websocket(低时延), 失败降级 polling(可靠性兜底)
    // 2026-08-10 加固: upgrade/rememberUpgrade 确保握手后优先保持 websocket
    //   (避免每次握手都从 polling 重升级导致反复闪断); forceNew=false 复用长连接
    socket = io({
        transports: ["websocket", "polling"],
        upgrade: true,
        rememberUpgrade: true,
        forceNew: false,
        reconnection: true,
        reconnectionAttempts: Infinity,   // 无限重试(长时间运行, 不因网络抖动放弃)
        reconnectionDelay: 2000,          // 首次重连延迟 2s
        reconnectionDelayMax: 15000,      // 重连延迟上限 15s(指数退避封顶)
        randomizationFactor: 0.4,         // 抖动因子, 防多客户端同时重连风暴
        timeout: 20000,                   // 连接超时 20s
    });

    // P2-1 降级提示: WebSocket 断开时显示横幅, 重连后自动隐藏
    function setDegradeBanner(show) {
        let banner = document.getElementById("degradeBanner");
        if (show) {
            if (!banner) {
                banner = document.createElement("div");
                banner.id = "degradeBanner";
                banner.textContent = "⚠ 实时性已降级：WebSocket 断开，数据刷新已切换为轮询模式（约 10~15s）";
                // 2026-08-11 优化: 横幅下移到顶栏(62px)之下 + pointer-events:none, 不再遮挡顶栏按钮与内容.
                Object.assign(banner.style, {
                    position: "fixed", top: "62px", left: "0", right: "0", zIndex: "9998",
                    background: "var(--accent-yellow, #f5a623)", color: "#1a1a1a",
                    textAlign: "center", padding: "6px 12px", fontSize: "13px",
                    fontWeight: "600", boxShadow: "0 2px 6px rgba(0,0,0,0.25)",
                    pointerEvents: "none"
                });
                document.body.appendChild(banner);
            }
        } else if (banner) {
            banner.remove();
        }
    }

    socket.on("connect", () => {
        setDegradeBanner(false);
        // 2026-08-20 修复(#6): WS 恢复 → 轮询回到 15s 心跳频率
        if (_wsPollFastMode) {
            _wsPollFastMode = false;
            try { _rescheduleCommPoll(); } catch (e) {}
        }
        addLog("[连接] WebSocket 已建立", "log-success");
        addOperationLog("WebSocket 已连接", "success");
        // 2026-08-10 稳定性优化: 重连成功后立即拉一次 /api/status,
        //   不等下一帧推送(推送间隔 10s), 缩短"已连上但数据还是旧"的窗口
        try { updateCommStatus(); } catch (e) {}
    });

    socket.on("disconnect", () => {
        setDegradeBanner(true);
        // 2026-08-20 修复(#6): cloudflared 隧道 WebSocket 长连接不稳定(stream canceled),
        //   断开期间加速轮询到 5s, 把数据滞后从 15s 压到 ~5s; 重连后自动回 15s.
        _wsPollFastMode = true;
        try { _rescheduleCommPoll(); } catch (e) {}
        addLog("[连接] WebSocket 已断开, 已降级为快速轮询(5s)", "log-error");
        addOperationLog("WebSocket 已断开，实时性降级", "error");
    });

    socket.on("bms_status", (data) => {
        // 2026-08-20 修复(#2 断电第一时间离线): 固件 EMQX 通道 LWT 遗嘱在设备断电/
        //   断网瞬间由 broker 发布到此事件。收到离线标志立即刷新状态与 UI——
        //   不等 15s 轮询, 前端秒级转离线(含顶部状态点/通信卡片/曲线灰线)。
        if (data && typeof data === "object") {
            const off = (data.device_online === false) || (data.no_data === true);
            if (off) {
                try {
                    updateDeviceOnlineUI(false, "OFFLINE");
                    updateDataSource(true, null, "设备离线(遗嘱)");
                    // 立即拉一次 /api/status, 让 emqx_age_ms/device_online 落到状态数据
                    updateCommStatus();
                } catch (e) { console.warn("bms_status 离线处理 fail:", e); }
            } else if (data.device_online === true || data.device_status === "ONLINE") {
                // 2026-08-21 修复(#5 实时性): 设备恢复上线时秒级转在线——
                //   原实现只处理离线分支, 在线恢复只能等 15s 轮询(实际 ~1min)。
                //   固件重连后 broker 发布 {device_online:true} 到本主题, 立即刷新 UI。
                try {
                    updateDeviceOnlineUI(true, "ONLINE");
                    updateDataSource(true, null, "设备在线");
                    updateCommStatus();
                } catch (e) { console.warn("bms_status 在线处理 fail:", e); }
            }
        }
    });

    socket.on("bms_data", (data) => {
        // 2026-08-14: 捕获 OTA 设备阶段回报(与 handleBmsData 解耦,
        // 避免其内部 no_data 等分支提前 return 导致漏捕获)
        if (data && typeof data === "object") {
            const st = data.ota_stage || (data.data && data.data.ota_stage) || "";
            const pr = (data.ota_progress != null) ? data.ota_progress
                     : (data.data && data.data.ota_progress != null ? data.data.ota_progress : null);
            const er = data.ota_error || (data.data && data.data.ota_error) || "";
            if (st) _otaDeviceStage = st;
            if (pr != null) _otaProgress = pr;   // 先更新进度/错误, 再触发回验(避免渲染滞后一帧)
            if (er) _otaError = er;
            if (st) { try { _checkOtaVerify(); } catch (e) {} }   // 设备阶段变化 → 触发 OTA 状态机回验
        }
        handleBmsData(data);
    });

    // ===== 2026-08-18 实时性整改: 高速精简帧(100ms) =====
    // 固件每 100ms 发一帧 bms/<id>/fast(QoS0), 只含曲线核心量(soc/pack_v/current/temp/fault),
    // 无 cells/CRC 等全量字段 → 本分支只推实时曲线 + 延迟数字, 不触发整页重绘/落库
    // (全量帧 bms_data 每 7s 仍负责 KPI 卡片/表格/历史; 高频帧与全量帧互不覆盖)
    socket.on("bms_fast", (data) => {
        if (!data || typeof data !== "object") return;
        // 延迟计算: fast 帧自带 Unix ms 时间戳(未同步时回退 boot tick, 前端二次过滤)
        const MIN_UNIX_MS = 946684800 * 1000;   // 2000-01-01, 与 handleBmsData 一致
        const _ts = Number(data.timestamp || 0);
        const _tsMs = (_ts > 0 && _ts < 1e12) ? _ts * 1000 : _ts;   // s → ms
        let ageMs = 0;
        if (_tsMs > MIN_UNIX_MS) ageMs = Math.max(0, Date.now() - _tsMs);
        window.__devAgeMs = ageMs;
        // 2026-08-21 修复(#5 实时性): 设备恢复后第一条 fast 帧(100ms)即转在线——
        //   固件重连后不发布 device_online:true(bms_status 在线分支不会触发),
        //   原恢复在线依赖 7s telemetry 帧(handleBmsData)或 15s 轮询, 实时性不足。
        //   fast 帧每 100ms 一帧, 收到新鲜帧(ageMs<=OFFLINE_MS)即证明设备在线,
        //   立即刷新顶部状态点(仅当前显示离线时执行, 避免每帧重复设置)。
        try {
            const _dot = $("connStatusDot");
            if (_dot && !_dot.classList.contains("online") && ageMs > 0 && ageMs <= 10 * 1000) {
                updateDeviceOnlineUI(true, "ONLINE");
                updateDataSource(true, null, "设备在线(fast)");
            }
        } catch (e) { console.warn("bms_fast 在线恢复 fail:", e); }
        // 更新延迟数字(与 handleBmsData 同一元素, 高频帧下更灵敏)
        if ($("latency")) {
            if (ageMs <= 0) {
                $("latency").textContent = "--";
            } else if (ageMs < 1000) {
                $("latency").textContent = ageMs + "ms";
            } else if (ageMs < 60 * 1000) {
                $("latency").textContent = (ageMs / 1000).toFixed(1) + "s";
            } else {
                $("latency").textContent = (ageMs / 60000).toFixed(1) + "min";
            }
        }
        // 实时曲线: 节流 200ms(100ms 帧太密, Chart.js update 吃不消), 仅主图可见时推
        // fast 帧无 cells/selectedCellIndex → pushMainChart 走 pack_v 分支(与全量帧同源算法)
        _gateRun("fastchart", 200, function () {
            if (_elVisible("mainChart")) pushMainChart(data);
        });
        // ===== 2026-08-18 实时性整改: KPI 大数字也随 100ms 高速帧刷新 =====
        //   原来 KPI(电压/电流/SOC)只随 7s 全量帧 bms_data 更新 → 前端看起来"不是亚秒级".
        //   现用 fast 帧精简字段节流刷新数字(300ms), 与全量帧互不覆盖:
        //   fast 帧字段: pack_v/current/soc/soh/temp_max/temp_min/fault(无 cells/CRC 等)
        _gateRun("fastkpi", 300, function () {
            try {
                const _packMv = Number(data.pack_v || 0);
                const _packV = _packMv / 1000;
                if (_packV > 0) {
                    const _vInt = Math.floor(_packV);
                    const _vDec = Math.round((_packV - _vInt) * 10);
                    if ($("voltInt")) $("voltInt").textContent = _vInt;
                    if ($("voltDec")) $("voltDec").textContent = _vDec;
                }
                const _curA = Number(data.current || 0) / 1000;
                const _curAbs = Math.abs(_curA);
                const _cf = _curAbs.toFixed(2).split(".");
                if ($("currInt")) $("currInt").textContent = _cf[0] || "0";
                if ($("currDec")) $("currDec").textContent = _cf[1] || "00";
                const _soc = Number(data.soc || 0);
                if ($("socVal")) $("socVal").textContent = Math.round(_soc);
                if ($("socBar")) $("socBar").style.width = Math.min(100, Math.max(0, _soc)) + "%";
                if ($("sfSoc")) $("sfSoc").textContent = Math.round(_soc) + "%";
                if ($("sfBar")) $("sfBar").style.width = Math.min(100, Math.max(0, _soc)) + "%";
                const _soh = Number(data.soh || 0);
                if ($("sohVal")) $("sohVal").textContent = Math.round(_soh);
                if ($("sohBar")) $("sohBar").style.width = Math.min(100, Math.max(0, _soh)) + "%";
                if (_packV > 0) {
                    const _pwr = Math.abs(_packV * _curA);
                    if ($("pwrVal")) $("pwrVal").textContent = Math.round(_pwr);
                    if ($("pwrBar")) $("pwrBar").style.width = Math.min(100, _pwr / 1500 * 100).toFixed(0) + "%";
                }
                const _tAvg = (Number(data.temp_max || 0) + Number(data.temp_min || 0)) / 2 / 10;
                const _avgEl = $("avgTemp");
                if (_avgEl) _avgEl.textContent = _tAvg > 0 ? _tAvg.toFixed(1) + "°" : "--";
            } catch (e) { console.warn("fastkpi:", e); }
        });
    });

    socket.on("bms_fault", (data) => {
        addLog("[告警] 收到故障告警: 0x" + Number(data.fault || 0).toString(16).toUpperCase(), "log-error");
        renderAlertList(data.fault);
    });

    socket.on("mqtt_status", (data) => {
        // ===== mqtt_status 语义: 区分云端与设备两种连接状态
        // data.cloud_accessible / data.connected : 后端 Dashboard -> 华为云 REST API
        // data.device_online                     : ESP32 设备 MQTT -> 华为云 是否 ONLINE
        // data.device_status                     : ONLINE/OFFLINE/FROZEN/INACTIVE...
        const cloudOk = !!(data.cloud_accessible !== undefined ? data.cloud_accessible : data.connected);
        const isOnline = !!data.device_online;
        addLog("[状态] 云端: " + (cloudOk ? "已连接" : "未连接")
             + " · 设备: " + (isOnline ? "在线" : ("离线(" + (data.device_status || "--") + ")")),
             cloudOk ? "log-info" : "log-error");

        /* 同步设备在线状态 UI (导航栏 + 页脚) */
        if (typeof data.device_online !== "undefined") {
            _otaDeviceOnline = isOnline;          // 维护 OTA 回验用的在线标志
            updateDeviceOnlineUI(isOnline, data.device_status || null);
            // 2026-08-08: 设备状态变化时同步刷新设备管理表格(跟随华为云实时数据)
            if (typeof loadDevices === "function") {
                try { loadDevices(); } catch (e) {}
            }
            try { _checkOtaVerify(); } catch (e) {}   // 设备回联 → 触发 OTA 成功回验
        }

        /* Q2 修复: 设备离线时 **不清零** KPI/曲线/单体数据 → 保留最后一次有效值显示为水平直线
           之前清零违反用户期望: "断网断电时应由存在磁盘的最后一条数据上传网页 → 显示直线, 不应归0" */
        const devOffline = (typeof data.device_online === "boolean") && !data.device_online
                        && (typeof data.cloud_accessible === "boolean" ? data.cloud_accessible : true);
        if (devOffline) {
            // 只更新离线状态提示, 数据仍保留 lastPayload 的最后一次有效值 (DB缓存)
            // 这样用户看到的是断网前最后一次数据, 曲线保持水平直线 (符合期望)
            if ($("latency")) $("latency").textContent = "--";
            updateDataSource(true, null, "设备离线(最后缓存)");
        }

        /* ===== MQTT KPI 卡片: 显示 ESP32 设备是否连上 MQTT(用户关心的) ===== */
        const mqttVal = $("mqttVal");
        const mqttDetail = $("mqttDetail");
        if (mqttVal) {
            if (isOnline) {
                mqttVal.textContent = "已连接";
                mqttVal.style.color = "var(--accent-green)";
            } else {
                // 区分: 云端访问不了 (黄色 连接中...) vs 云端OK但设备离线 (红色 未连接)
                if (!cloudOk) {
                    mqttVal.textContent = "云端未就绪";
                    mqttVal.style.color = "var(--accent-yellow)";
                } else {
                    mqttVal.textContent = "未连接";
                    mqttVal.style.color = "var(--accent-red)";
                }
            }
        }
        const row1b = $("mqttRow1");
        const row2b = $("mqttRow2");
        const mode = data.poll_mode === "online" ? "在线" :
                     data.poll_mode === "offline" ? "离线" :
                     data.poll_mode === "night" ? "夜间" : "冷启动";
        const devText = (data.device_status && data.device_status !== "UNKNOWN")
            ? ("设备: " + data.device_status)
            : (isOnline ? "设备: ONLINE" : "设备: 未知");
        const cloudText = cloudOk ? "云端: 正常" : "云端: 异常";
        const _r1b = cloudText + " | " + devText + " | 模式: " + mode;
        const _r2b = "区域: " + (data.region || "--") + " | 记录: " + (data.db_count || 0);
        if (row1b && row2b) {
            row1b.textContent = _r1b;
            row2b.textContent = _r2b;
        } else if (mqttDetail) {
            mqttDetail.textContent = _r1b + " | " + _r2b;
        }
        /* ===== 页脚 MQTT ===== */
        const fMqtt = $("footerMqtt");
        if (fMqtt) {
            if (cloudOk) {
                fMqtt.textContent = "📡 云端: 已连接 (" + (data.region || "--") + ") · 设备: "
                    + (isOnline ? "在线" : ("离线(" + (data.device_status || "--") + ")"));
                fMqtt.style.color = isOnline ? "var(--text-primary)" : "var(--accent-yellow)";
            } else {
                fMqtt.textContent = "📡 云端: 未连接 (访问华为云失败)";
                fMqtt.style.color = "var(--accent-red)";
            }
        }
        /* ===== 页脚 DB 数量 ===== */
        const fDb = $("footerDbCount");
        if (fDb && (data.db_count || data.db_count === 0)) {
            fDb.textContent = "📊 记录: " + data.db_count;
        }
        /* ===== 倒计时 ===== */
        if (data.next_query_in && data.next_query_in > 0) {
            nextQuerySeconds = data.next_query_in;
            lastCountdownTime = Date.now();
            const info = $("nextQueryInfo");
            if (info) {
                const modeLabel = data.poll_mode === "online" ? "[在线模式]" :
                                  data.poll_mode === "offline" ? "[离线模式]" :
                                  data.poll_mode === "night" ? "[夜间模式]" : "[冷启动模式]";
                info.textContent = modeLabel + " 下次: " + data.next_query_in + "s";
            }
        }
    });

    socket.on("cmd_resp", (data) => {
        const name = data.command_name || data.cmd || "unknown";
        const ok = data.ok;
        let detail = data.msg || data.status || (data.result ? JSON.stringify(data.result).slice(0, 100) : "");
        addLog("[响应] " + name + " -> " + (ok ? "成功" : "失败") + ": " + detail, ok ? "log-success" : "log-error");
        // 2026-08-21 命令回执真确认(#2): 设备经 EMQX bms/<id>/cmd/ack 回执,
        //   后端消费腿转发 cmd_resp 事件。收到回执即代表设备已执行, 清除
        //   15s 轮询回比(pending verify), 避免误报"设备未响应/执行失败"。
        if (_pendingCmdVerify && _pendingCmdVerify.cmd === name) {
            if (ok) {
                addLog("[响应] ✅ " + name + " 设备已回执确认执行", "log-success");
            } else {
                addLog("[响应] ⚠️ " + name + " 设备回执执行失败: " + detail, "log-error");
                showToast(name + " 设备执行失败: " + detail, "error");
            }
            _pendingCmdVerify = null;
        }
        // 控制开关状态若已由设备回执确认, 前端回比(charge_mos)自然一致, 无需额外处理
    });

    socket.on("bms_info", (data) => {
        if (data && (data.info || data.firmware)) {
            // 兼容两种负载: {"info":{"firmware":...}} 与扁平 {"firmware":...}
            const fw = (data.info && data.info.firmware) || data.firmware || "--";
            _cachedFirmware = fw;
            addLog("[信息] 固件: " + fw, "log-info");
            const sfFw = $("sfFw");
            if (sfFw) sfFw.textContent = fw;
            const otaCur = $("otaCurVer");
            if (otaCur && fw && fw !== "--") otaCur.textContent = fw;
            try { _checkOtaVerify(); } catch (e) {}   // 固件版本回报 → 触发 OTA 成功回验
        }
    });

    // 2026-08-11: 设备断网缓存补传完成 → 后端已按时间戳回填历史库,
    //   自动刷新当前激活的历史视图(主曲线/16路/历史趋势), 补全断网时段
    socket.on("history_updated", (data) => {
        addLog("[历史] 离线补传数据已回填, 刷新历史曲线", "log-info");
        try {
            // 仅刷新处于"历史模式"的视图, 实时滚动视图不受影响
            if (mainChartRange === "6h" || mainChartRange === "24h" || mainChartRange === "7d") {
                loadMainChartHistory(mainChartRange);
            }
            if (cellVoltRange === "1h" || cellVoltRange === "6h" || cellVoltRange === "24h" || cellVoltRange === "7d") {
                loadCellVoltHistory(cellVoltRange);
            }
            if (historyRange) {
                loadHistoryChart();
            }
        } catch (e) { /* 刷新异常忽略 */ }
    });
}

// ============================================================
// 全局 fetch 拦截: API 返回 401 时自动跳转登录页
// ============================================================
const _originalFetch = window.fetch;
window.fetch = function(url, options) {
    return _originalFetch.apply(this, arguments).then(response => {
        if (response.status === 401) {
            window.location.href = "/login";
            // 2026-08-19 修复(C6): 原实现返回永不 resolve 的 Promise, 下游 .finally()
            //   永不执行 → 按钮卡死"刷新中/诊断中"。改为 resolve 一个伪 401 响应对象:
            //   - 下游 .then(r => r.json()) 拿到 {ok:false,msg:"未登录"} 走失败分支
            //   - 下游 .finally() 正常执行恢复按钮
            //   - 不再出现"Unexpected token <"(不再解析 401 的 HTML 登录页)
            return {
                ok: false,
                status: 401,
                statusText: "Unauthorized",
                json: () => Promise.resolve({ ok: false, msg: "未登录(401)" }),
            };
        }
        // 2026-09-16 安全配套: 后端拦截默认密码用户(403 must_change_password)
        // → 弹系统级改密框(openChangePwdModal, 复用设置页同一弹窗), 改密成功走 relogin
        if (response.status === 403) {
            try {
                response.clone().json().then(j => {
                    if (j && j.must_change_password && typeof openChangePwdModal === "function"
                        && !document.getElementById("changePwdModal")?.classList.contains("show")) {
                        showToast("仍在使用默认密码, 为保障安全请先修改密码", "warn");
                        openChangePwdModal();
                    }
                });
            } catch (e) { /* 非JSON响应, 忽略 */ }
        }
        return response;
    });
};

// ============================================================
// 初始化
// ============================================================
function init() {
    // 企业级导航委托: 统一接管所有 [data-view] 元素的跳转(含 SOC-SOH 卡片)
    try { ViewRouter.init(); } catch (e) { addLog("[初始化] 导航委托失败: " + e.message, "log-error"); }
    // 2026-08-11: 启动时应用上次保存的主题(日光/夜视), 让切换持久化
    try {
        const _root = document.documentElement;
        const _saved = localStorage.getItem("bms_theme");
        const _theme = (_saved === "light" || _saved === "dark") ? _saved : (_root.getAttribute("data-theme") || "dark");
        _root.setAttribute("data-theme", _theme);
        const _tb = $("themeToggleBtn");
        if (_tb) _tb.textContent = _theme === "light" ? "🌙 夜视" : "🌞 日光";
    } catch (e) {}
    // 构建单体选择器(6 个按钮)
    try { buildCellSelector(); } catch (e) { addLog("[初始化] 单体选择器失败: " + e.message, "log-error"); }
    // 初始化图表(实时/历史/趋势/报表/SOH 分析)
    // 2026-08-08: 单个图表初始化失败不影响其他图表(try-catch 保护)
    try { initMainChart(); } catch (e) { addLog("[初始化] 实时曲线失败: " + e.message, "log-error"); }
    // 2026-08-11: 刷新后恢复主曲线时间范围(含历史范围)并预填充实时曲线, 避免刷新后曲线空白
    try { initMainChartRange(); } catch (e) { addLog("[初始化] 主曲线范围恢复失败: " + e.message, "log-error"); }
    try { initCellVoltChart(); } catch (e) { addLog("[初始化] 单体电压曲线失败: " + e.message, "log-error"); }
    try { initHistoryChart(); } catch (e) { addLog("[初始化] 历史曲线失败: " + e.message, "log-error"); }
    try { initTrendChart(); } catch (e) { addLog("[初始化] 趋势图失败: " + e.message, "log-error"); }
    try { initReportChart(); } catch (e) { addLog("[初始化] 报表图表失败: " + e.message, "log-error"); }
    try { initSohTrendChart(); } catch (e) { addLog("[初始化] SOH 分析图失败: " + e.message, "log-error"); }
    // 初始日志(从服务端加载持久化运行日志, 再追加启动消息)
    try { loadOperationLog(true); } catch (e) { addLog("[初始化] 加载运行日志失败: " + e.message, "log-error"); }
    addLog("[系统] Dashboard 已启动", "log-info");
    // 加载 Socket.IO(动态注入)
    loadSocketIO(() => {
        initSocket();
        addLog("[系统] Socket.IO 已加载", "log-success");
    });
    // 加载参数
    try { loadParams(); } catch (e) { addLog("[初始化] 参数加载失败: " + e.message, "log-error"); }
    // 加载历史曲线
    try { loadHistoryChart(); } catch (e) { addLog("[初始化] 历史曲线加载失败: " + e.message, "log-error"); }
    try { loadTrendChart("month"); } catch (e) { addLog("[初始化] 趋势图加载失败: " + e.message, "log-error"); }  // P2-5: SOC/SOH 趋势 (默认 30 天 = "月")
    // P2: 加载 SOH 趋势分析(默认 24h)
    try { loadSohTrend("24h", null); } catch (e) { addLog("[初始化] SOH 趋势加载失败: " + e.message, "log-error"); }
    // P2: 加载设备列表
    try { loadDevices(); } catch (e) { addLog("[初始化] 设备列表加载失败: " + e.message, "log-error"); }
    // ===== 2026-08-08 修复: 设备管理状态跟随华为云数据实时更新 =====
    //   原 loadDevices 仅初始化调用一次, 设备在线/串数/SOC 变化后表格不刷新;
    //   改为 30s 定时轮询 + SocketIO 状态事件触发刷新
    setInterval(() => { try { loadDevices(); } catch (e) {} }, 30000);
    // ===== Bug4 完善: 报表初始化 =====
    // 1) 先根据报表类型切换控件显示 + 填充今日日期
    if (typeof onReportTypeChange === "function") {
        try { onReportTypeChange(); } catch (_) {}
    }
    // 2) 自动生成今日日报预览(用户也可再手动点击"生成/今天"刷新)
    setTimeout(() => {
        try { if (typeof loadReport === "function") loadReport(); }
        catch (_) {}
    }, 600);
    // 2026-08-10 #29: 预填自定义 CSV 导出区间(默认最近 7 天)
    try {
        const cs = $("csvStart"), ce = $("csvEnd");
        if (cs && !cs.value) { const w = new Date(Date.now() - 7 * 86400000); cs.value = _fmtIsoDate(w); }
        if (ce && !ce.value) { ce.value = _fmtIsoDate(new Date()); }
    } catch (_) {}
    // 通信状态轮询(SocketIO 已实时推送, 15秒轮询作为心跳补充)
    // 2026-08-20 修复(#6 推送通道断开): cloudflared 隧道对 WebSocket 长连接不稳定
    //   (日志: stream canceled by remote / Unsolicited response on idle HTTP channel),
    //   断开期间前端只能靠轮询兜底。原固定 15s 轮询在 WS 断开时数据滞后最多 15s;
    //   改为自适应: WS 断开时加速到 5s 轮询, WS 重连后回到 15s, 最大限度补实时性.
    try { updateCommStatus(); } catch (e) { addLog("[初始化] 通信状态刷新失败: " + e.message, "log-error"); }
    let _commPollMs = 15000;
    function _rescheduleCommPoll() {
        const ms = (_wsPollFastMode ? 5000 : 15000);
        if (ms === _commPollMs) return;
        _commPollMs = ms;
        if (_commPollTimer) clearInterval(_commPollTimer);
        _commPollTimer = setInterval(() => { try { updateCommStatus(); } catch (e) {} }, _commPollMs);
    }
    _commPollTimer = setInterval(() => { try { updateCommStatus(); } catch (e) {} }, _commPollMs);
    // 倒计时定时器(每 1 秒更新一次)
    setInterval(updateCountdown, 1000);
    // 历史曲线定期刷新(每 60 秒)
    setInterval(loadHistoryChart, 60000);
    // 趋势图定期刷新(每 5 分钟, 保留上次 range: day/week/month)
    setInterval(() => { try { loadTrendChart(lastTrendRange || "month"); } catch (e) {} }, 300000);
    // SOH 衰减分析定期刷新(每 5 分钟)
    setInterval(() => { try { loadSohTrend(lastSohTrendRange || "24h", null); } catch (e) {} }, 300000);
    // 2026-08-13: 历史故障记录定时刷新(与实时告警互补, 刷新页面后仍能回溯)
    setInterval(() => { try { loadFaultHistory(); } catch (e) {} }, 60000);
    // 响应式
    try { updateResponsive(); } catch (e) {}
    window.addEventListener("resize", () => {
        updateResponsive();
        if (mainChart) mainChart.resize();
        if (cellVoltChart) cellVoltChart.resize();
        if (historyChart) historyChart.resize();
        if (trendChart) trendChart.resize();
        if (reportChart) reportChart.resize();
        if (sohTrendChart) sohTrendChart.resize();
    });
    // ===== 2026-08-09 工业加固: 页面销毁时释放资源(防内存泄漏/页面卡死) =====
    //   pagehide(切页/关页/刷新前)触发: 销毁图表实例 + 清理防抖定时器;
    //   SocketIO 由库自动断开; 长期运行后内存不持续增长
    window.addEventListener("pagehide", () => {
        try {
            if (_cellVoltRangeTimer) { clearTimeout(_cellVoltRangeTimer); _cellVoltRangeTimer = null; }
            [mainChart, cellVoltChart, historyChart, trendChart, reportChart, sohTrendChart].forEach(ch => {
                if (ch && typeof ch.destroy === "function") ch.destroy();
            });
            mainChart = cellVoltChart = historyChart = trendChart = reportChart = sohTrendChart = null;
        } catch (e) { /* 页面即将卸载, 销毁异常忽略 */ }
    });
    // 日志级别过滤
    const logLevel = $("logLevel");
    if (logLevel) logLevel.addEventListener("change", renderOperationLog);
    // 实时拉取一次 /api/status(以防 SocketIO 未就绪; 原 /api/latest 后端无此路由)
    fetch("/api/status")
        .then(r => r.json())
        .then(data => {
            if (data && Object.keys(data).length > 0) {
                // 直接传递后端 payload(已包含 no_data/data_source/device_status 等字段)
                // 仅在后端返回的是旧数据库行格式(无 cells 数组)时做兼容转换
                if (!data.cells && (data.c1 || data.c2)) {
                    data.cells = [];
                    for (let i = 1; i <= 32; i++) {
                        const v = data["c" + i];
                        if (v == null) break;
                        data.cells.push(v);
                    }
                    if (data.cells.length > 0) applySeriesNum(data.cells.length);
                    data.is_mock = false;
                }
                handleBmsData(data);
            }
        })
        .catch(() => {});
    // ===== 暴露关键函数到 window(供 HTML onclick 调用) =====
    // 控制开关类
    window.toggleSwitch = toggleSwitch;
    window.toggleBalance = toggleBalance;
    window.startCharging = startCharging;
    window.stopCharging = stopCharging;
    window.startDischarging = startDischarging;
    window.stopDischarging = stopDischarging;
    window.rebootDevice = rebootDevice;
    window.resetFadeCap = resetFadeCap;
    window.emergencyStop = emergencyStop;
    // 参数/阈值类
    window.saveBatteryParams = saveBatteryParams;
    window.saveThresholds = saveThresholds;
    window.resetThresholds = resetThresholds;
    window.sendCommand = sendCommand;
    // 告警类
    window.ackAlert = ackAlert;
    window.dismissAlert = dismissAlert;
    window.ackAllAlerts = ackAllAlerts;
    // 均衡弹窗
    window.openBalanceModal = openBalanceModal;
    window.closeBalanceModal = closeBalanceModal;
    window.sendBalanceCmd = sendBalanceCmd;
    // 诊断弹窗
    window.runDiag = runDiag;
    window.openDiagModal = openDiagModal;
    window.closeDiagModal = closeDiagModal;
    // 图表/视图类
    window.setChartCell = setChartCell;
    window.scrollToSection = scrollToSection;
    window.setCellVoltRange = setCellVoltRange;
    window.setMainChartRange = setMainChartRange;
    window.setHistoryRange = setHistoryRange;
    window.filterCells = filterCells;
    window.sortTable = sortTable;
    // 系统/界面类
    window.manualRefresh = manualRefresh;
    window.switchView = switchView;
    window.toggleMobileMenu = toggleMobileMenu;
    window.mobNavSwitch = mobNavSwitch;
    window.toggleUserMenu = toggleUserMenu;
    // 2026-08-22 修复(#3): 删除 `window.clearLogs = clearLogs;` —— clearLogs 从未定义,
    //   init() 执行到此处抛 ReferenceError, 中断点之后全部失效:
    //   window.saveRules/savePush/fetchMe/loadRulesConfig 等未暴露 → 设置页无法保存,
    //   页脚版本徽章不更新(无 ✅). HTML 实际引用的是 clearLogsDisplay()(已定义), 此行为死代码.
    // 2026-08-09: 浏览器通知开关(HTML onclick 引用, 必须暴露到 window)
    window.toggleBrowserNotify = toggleBrowserNotify;
    // 2026-08-08: 修改密码弹窗(暴露到 window, HTML onclick 需要全局函数)
    window.openChangePwdModal = openChangePwdModal;
    window.closeChangePwdModal = closeChangePwdModal;
    window.submitChangePwd = submitChangePwd;
    // ===== P2 新增功能函数 =====
    window.exportCSV = exportCSV;               // 旧版快照导出
    window.exportRealCSV = exportRealCSV;       // P2: 真实历史数据 CSV 导出
    window.exportCsvRange = exportCsvRange;     // 2026-08-10 #29: 自定义区间 CSV 导出
    // OTA 升级
    window.otaCheck = otaCheck;
    window.otaUpgrade = otaUpgrade;
    window.otaPublish = otaPublish;                 // 2026-08-16: 固件发布(admin-only, HTML onclick)
    window.otaRefreshServerInfo = otaRefreshServerInfo;   // 2026-08-16: 刷新服务器固件信息
    try { otaRefreshServerInfo(); } catch (e) {}    // 页面加载即显示服务器固件版本/大小/时间
    // 报表生成
    window.loadReport = loadReport;
    window.exportReportCSV = exportReportCSV;
    window.exportReportPDF = exportReportPDF;
    window.exportReportRaw = exportReportRaw;
    window.onReportTypeChange = onReportTypeChange;
    window.setReportQuick = setReportQuick;
    // 多设备管理
    window.loadDevices = loadDevices;
    window.onDeviceSwitch = onDeviceSwitch;
    // 2026-08-09: 3D 电池组视图(HTML onclick 引用)
    window.setBattery3DView = setBattery3DView;
    // 2026-08-09 双主题: 日光/夜视切换(HTML onclick 引用)
    window.toggleTheme = toggleTheme;
    // SOH 趋势分析
    window.loadSohTrend = loadSohTrend;
    // 大趋势(日/周/月切换)
    window.loadTrendChart = loadTrendChart;
    // 操作审计日志
    window.loadAuditLog = loadAuditLog;
    // 2026-08-10 #28: 拉取当前角色(门控管理员可编辑配置)
    window.fetchMe = fetchMe;
    try { fetchMe(); } catch (e) {}
    // 2026-08-22 修复(#3): 配置开关整行可点击(文字/空白处也能切换), 在 init 时绑定
    try { initConfigRowClick(); } catch (e) { console.warn("initConfigRowClick", e); }
    // 2026-08-10 #28: 系统设置新增面板函数(供 HTML onclick 调用)
    window.refreshRulesPushStatus = refreshRulesPushStatus;
    window.loadRulesConfig = loadRulesConfig;
    window.saveRules = saveRules;
    window.loadPushConfig = loadPushConfig;
    window.savePush = savePush;
    addLog("[系统] 初始化完成, P2 控制功能已就绪", "log-success");
    // 2026-08-22 修复(#4): JS 版本徽章——页面加载的 app.js 执行到此说明 JS 正常运行,
    //   在页脚徽章(由 index.html 模板注入 asset_ver 渲染)追加 ✅ 确认执行成功,
    //   供用户排查"网页端修复不生效"是否为浏览器缓存旧 JS.
    //   注: app.js 是静态文件不经过 Jinja2, 此处读取徽章现有文本而非硬编码版本号.
    try {
        const _badge = $("jsVersionBadge");
        if (_badge && _badge.textContent.indexOf("✅") < 0) {
            _badge.textContent = _badge.textContent.replace(/\s*$/, "") + " ✅";
            _badge.style.color = "var(--accent-green)";
        }
    } catch (e) {}
}

// ============================================================
// 2026-08-10 #28: 系统设置新增面板 —— 联动规则 / 告警推送 / 审计日志
// ============================================================
window._IS_ADMIN = false;     // 当前是否管理员(由 /api/me 决定)
window._ME_LOADED = false;    // 2026-08-22 修复(#3): /api/me 是否已成功返回
                              //   (角色未确认前 applyConfigGating 不禁用 checkbox,
                              //    避免用户切到设置页时 fetchMe 异步未返回而误禁用)
window._RULE_CFG = null;      // 缓存规则配置(用于保存时补全未改字段)
window._PUSH_CFG = null;      // 缓存推送配置

// 2026-08-22 修复(#3): 配置开关整行可点击 —— 原生只有 .switch 本体(46px)可切换,
//   用户点击文字/行内空白无反应("开关无法打开"). 事件委托: 点击 .cfg-check-row 行内
//   非 .switch 区域时手动触发内部 checkbox 切换; 点击 .switch 本体由 label 原生处理, 不重复.
function initConfigRowClick() {
    document.querySelectorAll(".cfg-check-row").forEach(function (row) {
        if (row._rowClickBound) return;
        row._rowClickBound = true;
        row.addEventListener("click", function (e) {
            if (e.target && e.target.closest && e.target.closest(".switch")) return; // 原生 label 已切换
            const cb = row.querySelector('input[type="checkbox"]');
            if (cb) cb.click();
        });
    });
}

function fetchMe() {
    return fetch("/api/me").then(r => r.json()).then(d => {
        if (d && d.ok) {
            window._IS_ADMIN = !!d.is_admin;
            window._CUR_ROLE = d.role || "viewer";
            window._LOGIN_USER = d.user || window._LOGIN_USER;
            window._ME_LOADED = true;   // 2026-08-22 修复(#3): /api/me 已确认, 门控生效
            // 2026-08-22 诊断(#4): 徽章直接显示真实角色/用户, 用于定位"admin 仍无法设置"
            const _rlab = { admin: "管理员", operator: "操作员", viewer: "只读用户" };
            try {
                const _b = $("jsVersionBadge");
                if (_b) {
                    _b.title = "角色: " + (_rlab[d.role] || d.role) + " | 用户: " + window._LOGIN_USER + " | _ME_LOADED=true";
                    if (_b.textContent.indexOf("✅") < 0) _b.textContent = _b.textContent.replace(/\s*$/, "") + " ✅";
                    _b.textContent += " [" + (_rlab[d.role] || d.role) + "]";
                    _b.style.color = "var(--accent-green)";
                }
            } catch (e) {}
            const rl = { admin: "管理员", operator: "操作员", viewer: "只读用户" };
            const roleEl = $("userRole");
            if (roleEl && rl[d.role]) roleEl.textContent = rl[d.role];
            // 2026-08-22 修复(#2): avatar title 同步为真实登录用户名(原硬编码 "admin")
            const avEl = $("avatar");
            if (avEl) {
                avEl.title = window._LOGIN_USER || "";
                avEl.textContent = (window._LOGIN_USER || "A").charAt(0).toUpperCase();
            }
            const tag1 = $("rulesEditTag"), tag2 = $("pushEditTag");
            if (tag1) tag1.textContent = d.is_admin ? "" : "（仅管理员可编辑）";
            if (tag2) tag2.textContent = d.is_admin ? "" : "（仅管理员可编辑）";
            // 2026-08-16: OTA 固件发布面板仅 admin 可见
            const pub = $("otaPublishPanel");
            if (pub) pub.style.display = d.is_admin ? "" : "none";
            // 2026-08-21: 用户管理卡片仅 admin 可见, 管理员进入设置时自动加载用户列表
            const umg = $("userMgmtCard");
            if (umg) umg.style.display = d.is_admin ? "" : "none";
            if (d.is_admin) {
                try { loadUsers(); } catch (e) { addLog("[用户] 初始化加载失败: " + e.message, "log-error"); }
            }
            // 2026-08-22 修复(#3): 规则/推送控件门控抽为独立函数, 此处与 switchView("settings")
            //   都调用 —— 确保不依赖 /api/me 异步时序, checkbox 状态始终按真实角色正确设置
            applyConfigGating();
            // 2026-08-21 角色门控(#4): viewer 只读(禁控制面板), 每次拉取后重应用
            try { applyRoleGating(); } catch (e) { console.warn("role gating:", e); }
        }
    }).catch(() => {
        // 2026-08-22 诊断(#4): /api/me 失败(会话超时/网络)时在徽章 title 标记, 便于定位
        try {
            const _b = $("jsVersionBadge");
            if (_b) _b.title = "⚠️ /api/me 获取失败(可能会话超时, 请重新登录)";
        } catch (e) {}
    });
}

// 2026-08-22 修复(#3): 规则/推送配置控件按角色门控 —— admin 显式恢复,
//   非 admin 禁用并隐藏保存按钮. 与 fetchMe 分离, 进入设置页时同步调用,
//   避免"点击 checkbox 无响应"(控件残留 disabled 状态).
function applyConfigGating() {
    // 2026-08-22 修复(#3 终版): 配置控件**永远可点击**, 权限完全由后端 role_required 兜底
    //   (非 admin 保存时后端返回 403, 前端提示"仅管理员可修改").
    //   历史: 前端 disabled 门控引发多轮"admin 无法设置"问题(时序/缓存导致误禁用),
    //   且与"徽章显示 [管理员] 但 checkbox 无法点击"矛盾无法自洽 —— 前端只做 UX,
    //   安全边界必须且已经由后端保证. 此函数保留仅用于: 保存按钮对非 admin 隐藏 + 提示文案.
    const isAdmin = (window._IS_ADMIN === true);
    const b1 = $("rulesSaveBtn"); if (b1) b1.style.display = isAdmin ? "" : "none";
    const b2 = $("pushSaveBtn"); if (b2) b2.style.display = isAdmin ? "" : "none";
    const tag1 = $("rulesEditTag"); if (tag1) tag1.textContent = isAdmin ? "" : "（仅管理员可编辑）";
    const tag2 = $("pushEditTag"); if (tag2) tag2.textContent = isAdmin ? "" : "（仅管理员可编辑）";
}

function refreshRulesPushStatus() {
    Promise.all([fetch("/api/rules").then(r=>r.json()), fetch("/api/push_config").then(r=>r.json())])
    .then(([rj, pj]) => {
        const box = $("rulesPushStatus");
        if (!box) return;
        const rc = (rj && rj.ok) ? rj.config : {};
        const pc = (pj && pj.ok) ? pj.config : {};
        const chans = [];
        if (pc.webhook) chans.push("Webhook");
        if (pc.sct_key) chans.push("Server酱");
        if (pc.pp_token) chans.push("PushPlus");
        const dot = (ok) => ok
            ? '<span class="status-dot" style="background:var(--accent-green);display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px"></span>'
            : '<span class="status-dot" style="background:var(--accent-red);display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px"></span>';
        box.innerHTML =
            statItem(dot(rc.enabled) + "联动规则", rc.enabled ? "已启用" : "已停用") +
            statItem("低 SOC 阈值", (rc.low_soc != null ? rc.low_soc : "--") + " %") +
            statItem(dot(rc.overvolt) + "单体过压保护", rc.overvolt ? "开启" : "关闭") +
            statItem(dot(rc.temp_prot) + "温度保护", rc.temp_prot ? "开启" : "关闭") +
            statItem("触发消抖", (rc.debounce_s != null ? rc.debounce_s : "--") + " s") +
            statItem(dot(pc.enabled) + "告警推送", pc.enabled ? "已配置" : "未配置") +
            statItem("推送通道", chans.length ? chans.join(" / ") : "无") +
            statItem(dot(pc.online_notify) + "上下线通知", pc.online_notify ? "开启" : "关闭");
        // 同步缓存(供保存时增量补全)
        window._RULE_CFG = rc; window._PUSH_CFG = pc;
    })
    .catch(err => {
        const box = $("rulesPushStatus");
        if (box) box.innerHTML = '<div style="color:var(--accent-red);padding:8px">状态加载失败: ' + htmlEsc(String(err)) + '</div>';
    });
}
function statItem(label, val) {
    return '<div style="padding:10px 12px;background:var(--bg-secondary);border:1px solid var(--border-color);border-radius:8px">' +
           '<div style="font-size:11px;color:var(--text-secondary);margin-bottom:4px">' + label + '</div>' +
           '<div style="font-size:14px;font-weight:600;color:var(--text-primary)">' + val + '</div></div>';
}

// 2026-08-22 修复(#3): 规则/推送表单"仅首次填充"标志——switchView("settings")
//   每次进入设置页都会调 loadRulesConfig/loadPushConfig, 原实现无条件强制
//   checked=后端值, 用户取消打钩后一切走再回来又被勾回(表现为"无法取消打钩").
//   现仅首次填充 UI, 之后刷新只更新缓存, 不覆盖用户正在编辑的表单.
let _rulesUiInited = false;
let _pushUiInited = false;

function loadRulesConfig() {
    fetch("/api/rules").then(r=>r.json()).then(d => {
        if (!d || !d.ok) return;
        window._RULE_CFG = d.config || {};
        if (_rulesUiInited) return;   // 已填充过, 不再覆盖用户编辑
        const c = d.config;
        if ($("ruleEnabled")) $("ruleEnabled").checked = !!c.enabled;
        if ($("ruleLowSoc")) $("ruleLowSoc").value = c.low_soc != null ? c.low_soc : "";
        if ($("ruleOvervolt")) $("ruleOvervolt").checked = !!c.overvolt;
        if ($("ruleTempProt")) $("ruleTempProt").checked = !!c.temp_prot;
        if ($("ruleDebounce")) $("ruleDebounce").value = c.debounce_s != null ? c.debounce_s : "";
        _rulesUiInited = true;
    }).catch(()=>{});
}

function saveRules() {
    // 2026-08-22 修复(#3): 保存前实时复查角色——原实现依赖页面加载时异步
    //   fetchMe() 设置的 _IS_ADMIN, 浏览器缓存旧 JS/时序未及时更新时误报
    //   "仅管理员可修改". 后端 role_required("admin") 仍兜底 403.
    if (!window._IS_ADMIN) {
        fetch("/api/me").then(r => r.json()).then(d => {
            window._IS_ADMIN = !!(d && d.ok && d.is_admin);
            if (window._IS_ADMIN) { doSaveRules(); }
            else showToast("仅管理员可修改联动规则", "error");
        }).catch(() => showToast("仅管理员可修改联动规则", "error"));
        return;
    }
    doSaveRules();
}
function doSaveRules() {
    const payload = {
        enabled: $("ruleEnabled") ? $("ruleEnabled").checked : undefined,
        low_soc: $("ruleLowSoc") && $("ruleLowSoc").value !== "" ? parseInt($("ruleLowSoc").value,10) : undefined,
        overvolt: $("ruleOvervolt") ? $("ruleOvervolt").checked : undefined,
        temp_prot: $("ruleTempProt") ? $("ruleTempProt").checked : undefined,
        debounce_s: $("ruleDebounce") && $("ruleDebounce").value !== "" ? parseInt($("ruleDebounce").value,10) : undefined,
    };
    const clean = {};
    Object.keys(payload).forEach(k => { if (payload[k] !== undefined) clean[k] = payload[k]; });
    const msg = $("rulesMsg");
    if (msg) msg.textContent = "保存中…";
    fetch("/api/rules", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(clean)
    }).then(r => r.json().then(j => ({status: r.status, j})))
    .then(({status, j}) => {
        if (msg) msg.textContent = (j && j.msg) || (status === 200 ? "已保存" : "保存失败");
        if (status === 200) { showToast("联动规则已保存并下发", "success"); refreshRulesPushStatus(); }
        else showToast((j && j.msg) || "保存失败", "error");
    }).catch(err => { if (msg) msg.textContent = "保存失败: " + err; });
}

function loadPushConfig() {
    fetch("/api/push_config").then(r=>r.json()).then(d => {
        if (!d || !d.ok) return;
        window._PUSH_CFG = d.config || {};
        if (_pushUiInited) return;   // 已填充过, 不再覆盖用户编辑
        const c = d.config;
        const applySecret = (id, val) => {
            const el = $(id); if (!el) return;
            const masked = !!(val && String(val).indexOf("***") >= 0);
            el.value = masked ? "" : (val || "");
            el.dataset.masked = masked ? "1" : "0";
            if (el.placeholder !== undefined) el.placeholder = masked ? "（已配置，留空保持不变）" : el.placeholder;
        };
        applySecret("pushWebhook", c.webhook);
        applySecret("pushSct", c.sct_key);
        applySecret("pushPp", c.pp_token);
        if ($("pushOnline")) $("pushOnline").checked = !!c.online_notify;
        _pushUiInited = true;
    }).catch(()=>{});
}

function savePush() {
    // 2026-08-22 修复(#3): 与 saveRules 对称——保存前实时复查角色, 避免缓存/时序误报
    if (!window._IS_ADMIN) {
        fetch("/api/me").then(r => r.json()).then(d => {
            window._IS_ADMIN = !!(d && d.ok && d.is_admin);
            if (window._IS_ADMIN) { doSavePush(); }
            else showToast("仅管理员可修改告警推送", "error");
        }).catch(() => showToast("仅管理员可修改告警推送", "error"));
        return;
    }
    doSavePush();
}
function doSavePush() {
    const secret = (id) => {
        const el = $(id);
        if (!el) return "__keep__";
        if (el.value.trim() === "") return el.dataset.masked === "1" ? "__keep__" : "";
        return el.value.trim();
    };
    const payload = {
        webhook: secret("pushWebhook"),
        sct_key: secret("pushSct"),
        pp_token: secret("pushPp"),
        online_notify: $("pushOnline") ? $("pushOnline").checked : undefined,
    };
    const clean = {};
    Object.keys(payload).forEach(k => { if (payload[k] !== undefined) clean[k] = payload[k]; });
    const msg = $("pushMsg");
    if (msg) msg.textContent = "保存中…";
    fetch("/api/push_config", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(clean)
    }).then(r => r.json().then(j => ({status: r.status, j})))
    .then(({status, j}) => {
        if (msg) msg.textContent = (j && j.msg) || (status === 200 ? "已保存" : "保存失败");
        if (status === 200) { showToast("告警推送已保存", "success"); refreshRulesPushStatus(); loadPushConfig(); }
        else showToast((j && j.msg) || "保存失败", "error");
    }).catch(err => { if (msg) msg.textContent = "保存失败: " + err; });
}

function loadAuditLog() {
    const limit = $("auditLimit") ? parseInt($("auditLimit").value,10) : 100;
    const filter = $("auditFilter") ? $("auditFilter").value.trim() : "";
    let url = "/api/audit_log?limit=" + limit;
    if (filter) url += "&cmd=" + encodeURIComponent(filter);
    fetch(url).then(r=>r.json()).then(d => {
        const body = $("auditLogBody");
        if (!body) return;
        if (!d || !d.ok || !d.data || !d.data.length) {
            body.innerHTML = '<tr><td colspan="5" style="color:var(--text-muted);padding:20px;text-align:center">暂无审计记录</td></tr>';
            return;
        }
        let html = "";
        d.data.forEach(row => {
            const t = fmtTime(row.recv_time);
            const res = row.result === "ok"
                ? '<span class="badge" style="background:rgba(16,185,129,0.15);color:var(--accent-green)">成功</span>'
                : '<span class="badge" style="background:rgba(239,68,68,0.15);color:var(--accent-red)">失败</span>';
            let paras = row.paras || "";
            if (typeof paras === "string" && paras.length > 90) paras = paras.slice(0,90) + "…";
            html += "<tr><td style='white-space:nowrap;font-variant-numeric:tabular-nums;color:var(--text-secondary)'>" + t + "</td>" +
                    "<td style='font-weight:600;color:var(--text-primary)'>" + esc(row.cmd) + "</td>" +
                    "<td style='color:var(--text-secondary)'>" + esc(row.source) + "</td>" +
                    "<td>" + res + "</td>" +
                    "<td style='color:var(--text-muted);max-width:320px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap' title='" + esc(row.paras||"") + "'>" + esc(paras) + "</td></tr>";
        });
        body.innerHTML = html;
    }).catch(err => {
        const body = $("auditLogBody");
        if (body) body.innerHTML = '<tr><td colspan="5" style="color:var(--accent-red);padding:20px;text-align:center">加载失败: ' + htmlEsc(String(err)) + '</td></tr>';
    });
}
function fmtTime(ts) {
    if (!ts) return "--";
    const d = new Date(ts * 1000);
    const p = (n) => (n<10?"0":"")+n;
    return d.getFullYear()+"-"+p(d.getMonth()+1)+"-"+p(d.getDate())+" "+p(d.getHours())+":"+p(d.getMinutes())+":"+p(d.getSeconds());
}
function esc(s) {
    if (s == null) return "";
    return String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]));
}

// 表格排序(供 th onclick 使用,如需)
function sortTable(colIdx) {
    const tbody = $("cellTableBody");
    if (!tbody) return;
    const rows = Array.from(tbody.rows);
    const dir = tbody.dataset.sortdir === "asc" ? -1 : 1;
    tbody.dataset.sortdir = dir === 1 ? "desc" : "asc";
    rows.sort((a, b) => {
        let av = a.cells[colIdx] ? a.cells[colIdx].textContent.trim() : "";
        let bv = b.cells[colIdx] ? b.cells[colIdx].textContent.trim() : "";
        const na = parseFloat(av), nb = parseFloat(bv);
        if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dir;
        return av.localeCompare(bv) * dir;
    });
    rows.forEach(r => tbody.appendChild(r));
}

// DOM 就绪后初始化
if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
} else {
    init();
}
