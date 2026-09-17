/**
 * @file    bms_config.h
 * @brief   BMS 全局配置项集中定义(魔法数收纳)
 * @author  BMS Team
 * @date    2026-08
 * @note    所有可调参数集中于此, 业务代码引用宏名, 禁止硬编码
 *          数值来源: 硬件配置说明.md
 */
#ifndef BMS_CONFIG_H
#define BMS_CONFIG_H

/* ====== 系统总体 ====== */
/* 2026-08-07: 串数泛化
 *   BMS_CELL_SERIES_NUM    = 实际串联数(改大即升级电池组, 如 16S/24S)
 *   BMS_MAX_CELL_SERIES_NUM = 编译期数组/缓冲区上限, 固定分配避免 ABI 变动,
 *                             后续扩串只需改 BMS_CELL_SERIES_NUM(≤MAX 即可)
 *   约束: BMS_CELL_SERIES_NUM <= BMS_MAX_CELL_SERIES_NUM <= 32(均衡掩码位宽)
 * 2026-08-09: 目标升级为 1~32S 自适应——MAX 提到 32 */
#define BMS_CELL_SERIES_NUM         16          // 电池实际串联数 (C1 仿制板 BQ76952 16S, used-cell-channels=0xFFFF)
#define BMS_MAX_CELL_SERIES_NUM     32          // 编译期最大串数(数组/缓冲/均衡掩码上限, 1~32S 自适应)
#define BMS_CELL_PARALLEL_NUM       1          // 并联数
#define BMS_CELL_CAPACITY_MAH       2500       // 单体容量, 单位 mAh
#define BMS_NOMINAL_VOLTAGE_MV      57600      // 标称总压, 单位 mV (3.6V x 16)
#define BMS_FULL_VOLTAGE_MV         67200      // 满充总压, 单位 mV (4.2V x 16)
#define BMS_CUTOFF_VOLTAGE_MV       44800      // 放电截止, 单位 mV (2.8V x 16)

/* =====================================================================
 * 硬件模块屏蔽开关 (缺硬件时置 0, 系统自动跳过该模块初始化与采集)
 * 用法: 在 bsp_xxx_init() 中判断, 若为 0 则直接返回 BMS_ERR_NOT_INIT
 *       在 app_tasks.c 中判断, 若为 0 则不创建对应任务
 * ===================================================================== */
#define HW_ENABLE_LTC6804           0          // 1=启用LTC6804电压采集AFE(SPI2)  0=未接线屏蔽(驱动保留)
#define HW_ENABLE_BQ76952           1          // 1=启用BQ76952电压采集AFE(I2C)  0=未接线屏蔽(替代LTC6804方案)
#define HW_ENABLE_INSULATION_DETECT 0          // 1=启用绝缘检测(不平衡电桥法) 0=硬件未接线屏蔽
#define HW_ENABLE_ACTIVE_BALANCE    0          // 1=启用主动均衡(能量转移,需外接电路) 0=未接线屏蔽
#define HW_ENABLE_BQ34Z100          0          // 1=启用BQ34Z100独立电量计  0=未接线屏蔽 (2026-08-25: C1 仿制板无 BQ34Z100, SOC 走 BQ76952 库仑计)
#define HW_ENABLE_TWAI              0          // 1=启用 TWAI CAN(ESP32-S3 内置控制器 + SN65HVD230 收发器) 0=屏蔽 (2026-08-25: 替代原 MCP2515 方案)
#define HW_ENABLE_RS485             0          // 1=启用RS485通信(Modbus RTU) 0=硬件预留未接线(2026-08-13: 已实现 bsp_rs485 + sys_modbus)

/* ====== RS485 / Modbus RTU 参数(2026-08-13 新增, 与硬件说明一致) ======
 * 行业标准: Modbus RTU 8N1, 帧间隔 3.5 字符, CRC16(MODBUS 多项式 0xA001)
 * 角色: Slave(从站), 默认站地址 1, 被上位机/PLC 轮询
 * 寄存器区: 40001~40050 保持寄存器(03/06/10 读写) + 30001~30050 输入寄存器(04 只读) */
#define BMS_RS485_BAUD_DEFAULT      9600       // 默认波特率(可经保持寄存器 40002 运行时修改)
#define BMS_MODBUS_SLAVE_ADDR       1          // 默认从站地址(可经保持寄存器 40001 修改)
#define BMS_MODBUS_RTU_TIMEOUT_MS   10         // 3.5 字符间隔判帧超时(9600bps≈4ms, 取10ms余量)
#define BMS_MODBUS_TASK_STACK       4096       // Modbus 任务栈
#define BMS_MODBUS_TASK_PRIO        4          // Modbus 任务优先级(低于采集/保护, 高于看门狗)
#define HW_ENABLE_SD_CARD           0          // 1=启用SD卡黑匣子          0=未接线屏蔽
#define HW_ENABLE_OLED              0          // 1=启用OLED显示(SH1106 0x3C, 与 BQ76952 0x08 并联 I2C 总线)  0=屏蔽 (2026-09-07: 用户不使用 OLED, 已关闭)
#define HW_ENABLE_RELAY             1          // 1=启用功率开关控制        0=屏蔽 (2026-08-25: 语义改为 BQ76952 CHG/DSG FET 控制, 替代继电器 GPIO)
#define HW_ENABLE_BUZZER           1          // 1=启用蜂鸣器(有源, GPIO11, 高电平有效, 2026-08-25 新增)  0=屏蔽
#define HW_ENABLE_LED               1          // 1=启用LED状态指示         0=未接线屏蔽 (2026-08-25: 红/绿双 GPIO 灯, 替代 WS2812)
#define HW_ENABLE_CURRENT_SENSE     1          // 1=启用电流采样            0=屏蔽 (2026-08-25: 数据源改为 BQ76952 库仑计, 替代 INA181 ADC)
#define HW_ENABLE_WIFI              1          // 1=启用WiFi+MQTT+OTA       0=未接线屏蔽
#define HW_ENABLE_BUTTON            0          // 1=启用按钮输入(BOOT键复用) 0=未接线屏蔽

/* ====== 按钮驱动参数 ====== */
#define BUTTON_DEBOUNCE_MS          20         // 消抖时间, 单位 ms
#define BUTTON_LONG_PRESS_MS        3000       // 长按判定阈值, 单位 ms
#define BUTTON_DOUBLE_CLICK_MS      400        // 双击间隔窗口, 单位 ms

/* ====== 按钮 UI / 菜单系统参数 ======
 * 单键 BOOT 键(GPIO0)复用, 组合三种事件实现完整交互:
 *   短按(<3s):    下一项 / 值+1 / 翻页
 *   双击(400ms):  确认 / 进入子菜单 / 保存
 *   长按(>=3s):   返回上级 / 取消 / 解除报警 / 关机
 */
#define OLED_PAGE_COUNT             5          // 显示模式总页数(Page0~3 数据页 + Page4 报警页)
#define OLED_AUTO_ROTATE_MS         3000       // 显示模式自动轮播间隔, 单位 ms(0=禁用自动轮播)
#define MENU_IDLE_TIMEOUT_MS        30000      // 菜单模式无操作超时退回显示, 单位 ms(0=禁用)
#define MENU_ITEM_COUNT             6          // 主菜单最大项数
#define PARAM_STEP_SMALL            1          // 微调步进(短按一步)
#define PARAM_STEP_BIG              10         // 快速步进(长按步进, 保留)

/* ====== 电压保护阈值(单位 mV) ====== */
#define CELL_OV_WARN_MV             4150       // 单体过压预警
#define CELL_OV_PROT_MV             4250       // 单体过压保护
#define CELL_OV_RECOVER_MV          4100       // 过压恢复
#define CELL_UV_WARN_MV             2900       // 单体欠压预警
#define CELL_UV_PROT_MV             2800       // 单体欠压保护
#define CELL_UV_RECOVER_MV          3000       // 欠压恢复

/* ====== 电流保护阈值(单位 mA, 与 sys_data 中 current_ma 一致) ======
 * 注意: 原 0.01A 放大100倍写法已弃用, 与代码中 mA 单位不一致, 改为直接用 mA
 * Bug1 修复: 恢复原厂阈值, 解决 3A 放电电流误触发保护(原测试值 2A 过低) */
#define CHG_OC_WARN_MA              4000       // 充电过流预警, 4A (充电电流为负)
#define CHG_OC_PROT_MA              5000       // 充电过流保护, 5A
#define DSG_OC_WARN_MA              8000       // 放电过流预警, 8A
#define DSG_OC_PROT_MA              10000      // 放电过流保护, 10A
#define SHORT_PROT_MA               30000      // 短路保护, 30A(硬件熔断器)

/* ====== 温度保护阈值(单位 0.1℃) ====== */
#define TEMP_OT_WARN_DC             500        // 过温预警, 50.0℃
#define TEMP_OT_PROT_DC             600        // 过温保护, 60.0℃
#define TEMP_OT_RECOVER_DC          450        // 过温恢复, 45.0℃ (迟滞 5℃)
#define TEMP_UT_WARN_DC             -100       // 低温预警, -10.0℃ (充电限流)
#define TEMP_UT_PROT_DC             -200       // 低温保护, -20.0℃ (充电禁止)
#define TEMP_UT_RECOVER_DC          0          // 低温恢复, 0.0℃
/* 热失控阈值单位: ℃/min (与 app_protection.c 中 calc_dtdt() 返回值一致)
 * 注意: 原值 30/100 是按 0.1℃/min 单位, 与代码单位不匹配, 已修正为 3.0/10.0 */
#define TEMP_DTDT_WARN              2.0f       // 温升速率预警, dT/dt > 2℃/min (热失控前兆)
#define TEMP_THERMAL_RUNAWAY_WARN   3.0f       // 热失控预警, dT/dt > 3℃/min
#define TEMP_THERMAL_RUNAWAY_ALARM  10.0f      // 热失控报警, dT/dt > 10℃/min

/* ====== 热失控 dV/dt 电压降速率检测(设计文档 4.1.3) ======
 * 热失控三要素: dT/dt(温升率) + dV/dt(电压降率) + 单体压差异常
 * dV/dt < -50mV/s: 电压快速下降(热失控前兆, 内部短路导致)
 * dV/dt < -100mV/s: 严重电压下降(热失控报警, 需立即锁定) */
#define DV_DT_WARN_MVPS             (-50.0f)   // 电压降速率预警, dV/dt < -50 mV/s
#define DV_DT_ALARM_MVPS            (-100.0f)  // 电压降速率报警, dV/dt < -100 mV/s

/* ====== 电压压差预警(单位 mV) ====== */
#define CELL_DV_WARN_MV             100        // 单体压差过大预警, ΔV >= 100mV
#define CELL_DV_RECOVER_MV          60         // 压差恢复, ΔV < 60mV (迟滞 40mV)

/* ====== 额定电流与持续过载(单位 mA) ======
 * 1C = 容量 mAh, 额定放电电流 = 1C = BMS_CELL_CAPACITY_MAH
 * 过载区间: 1.05~1.30 倍额定, 持续 > 2s 触发预警 */
#define RATED_CURRENT_MA            BMS_CELL_CAPACITY_MAH   // 额定电流 = 2500mA (1C)
#define OVERLOAD_WARN_RATIO         1.05f      // 过载预警起点(1.05倍额定)
#define OVERLOAD_PROT_RATIO         1.30f      // 过载保护起点(1.30倍额定, 超过则直接保护)
#define OVERLOAD_DURATION_MS        2000       // 过载持续时长阈值, 单位 ms

/* ====== SOC/SOH 预警 ====== */
#define SOC_LOW_WARN_PCT            15         // 电量过低预警, SOC <= 15%
#define SOC_LOW_RECOVER_PCT         20         // 电量恢复, SOC > 20% (迟滞 5%)
#define SOH_LOW_WARN_PCT            80         // 健康度衰减预警, SOH <= 80%

/* ====== 降额限流配置 ======
 * 预警触发时, 根据预警类型计算降额因子(0.0~1.0)
 * 不同预警类型的降额幅度(取最严格者, 即最小因子):
 *   过温预警:   70% (因子 0.7)
 *   过载预警:   50% (因子 0.5)
 *   低温预警:   50% (因子 0.5, 充电限流)
 *   低SOC预警:  60% (因子 0.6, 放电限流)
 *   压差预警:   80% (因子 0.8)
 *   温升速率:   50% (因子 0.5, 紧急降额) */
#define DERATING_OT_WARN            0.7f       // 过温预警降额
#define DERATING_OVERLOAD           0.5f       // 过载预警降额
#define DERATING_UT_WARN            0.5f       // 低温预警降额(充电)
#define DERATING_LOW_SOC            0.6f       // 低SOC降额(放电)
#define DERATING_CELL_DV            0.8f       // 压差过大降额
#define DERATING_DTDT               0.5f       // 温升速率降额

/* ====== 均衡参数 ====== */
#define BALANCE_THRESHOLD_MV        30         // 触发均衡: 高于均值 30mV
#define BALANCE_STOP_MV             10         // 停止均衡: 压差 < 10mV
#define BALANCE_TIMEOUT_MIN         30         // 均衡超时, 单位 min

/* ====== 电流采样(INA181 + 分流电阻) ====== */
#define SHUNT_RES_MOHM              5          // 分流电阻, 单位 mΩ
#define INA181_GAIN                 50         // INA181 放大倍数
#define INA181_VREF_MV              1650       // 零点电压, VCC/2, 单位 mV
#define ADC_VREF_MV                 3300       // ADC 参考电压, 单位 mV
#define ADC_RESOLUTION              4095       // 12 位 ADC 满量程
#define CURRENT_FILTER_SAMPLES      16         // 电流均值滤波采样数

/* ====== 温度采样(NTC) ====== */
#define NTC_R_REF_OHM               10000      // 分压电阻, 单位 Ω
#define NTC_R25_OHM                 10000      // 25℃ 标称阻值, 单位 Ω
#define NTC_B_VALUE                 3950       // B 值
#define NTC_TEMP_INVALID            -990.0f    // 温度无效哨兵值, 单位 0.1℃ (=-99.0℃, 与 bsp_temp_calc_dc 返回值单位一致)

/* ====== 绝缘检测参数(2026-08-09, 不平衡电桥法, 对标 GB/T 38661/ISO 6469) ======
 * 原理: 正/负极母线对地分别接入已知桥臂电阻 R0, 分时测上下桥臂电压,
 *       由两次测量解算 Rp(正极对地)/Rn(负极对地)绝缘电阻.
 * 国标要求: 直流回路绝缘电阻 > 100Ω/V(GB/T 18384.1), 交流 > 500Ω/V.
 * 当前硬件未接线(HW_ENABLE_INSULATION_DETECT=0), 软件框架与算法已就绪 */
#define INSULATION_SAMPLE_PERIOD_MS 1000       // 绝缘检测周期, 单位 ms
#define INSULATION_BRIDGE_OHM       1000000    // 桥臂电阻 R0, 单位 Ω (1MΩ, ≥500kΩ 国标下限)
#define INSULATION_ADC_VREF_MV      3300       // 采样 ADC 参考电压, 单位 mV
#define INSULATION_ADC_BITS         12         // 采样 ADC 位数(ESP32-S3 ADC1 12bit)
#define INSULATION_WARN_OHM_PER_V   100        // 预警阈值, Ω/V (GB/T 18384.1 直流 100Ω/V)
#define INSULATION_PROT_OHM_PER_V   50         // 保护阈值, Ω/V (预警的一半, 更保守)
#define INSULATION_SW_SETTLE_MS   5            // 切换桥臂(Q1)后等待调理电路(RC/运放)稳定的延时, ms
#define INSULATION_AVG_SAMPLES    8            // 每次 Vmid 采样的均值次数(抑制 ADC 噪声)
#define INSULATION_DIV_RATIO_X10  40           // 调理分压比×10(实际 Vmid = 调理后电压 ×4.0); 须与电路 Rd1/Rd2 实现一致
                                                 // 必须按实际串数与满充电压重设: 要求 Vbat_max/2 / (比值) ≤ 3.3V(ADC 上限)
                                                 //   例: 6S 满充 25.2V→Vmid≈12.6V→比率≥3.8(取4.0); 16S 需≥10.2; 32S 需≥20.4
                                                 //   另: 调理前端须把 Vmid 搬移到 0~3.3V 单端量程内(如中心偏置), 否则负摆幅会削波

/* ====== LTC6804 参数 ====== */
#define LTC6804_SPI_HZ              1000000    // 最大 SPI 速率 1MHz
#define LTC6804_ADC_CONV_MS         12         // 标准转换时间, 单位 ms
#define LTC6804_VLSB_UV             100        // 电压分辨率, 单位 μV
#define LTC6804_CELL_NUM            12         // 单芯片最大支持 12 串
/* 2026-08-09: 多芯片方案(共享 SPI2 + 独立片选, 方案A)支持 1~32S:
 *   16S=2 片 / 24S=2 片 / 32S=3 片(12+12+8)
 *   约束: BMS_CELL_SERIES_NUM <= LTC6804_CHIP_NUM * LTC6804_CELL_NUM */
#define LTC6804_CHIP_NUM            1          // LTC6804 芯片数量(16S=2 / 24S=2 / 32S=3)

/* 2026-08-19 修复(F6): 硬件实际可支持的串数上限(区别于编译期数组上限 BMS_MAX_CELL_SERIES_NUM=32)
 *   串数下发/设置若超过硬件能力, 会导致读取不到的串位出现虚假 0V 单体、误报警。
 *   按当前启用 AFE 推导: BQ76952 原生 3~16S / LTC6804 = 芯片数×12 / 无 AFE 用编译期串数 */
#if HW_ENABLE_BQ76952
#define BMS_HW_MAX_SERIES_NUM       16          // BQ76952 原生最大 16S
#elif HW_ENABLE_LTC6804
#define BMS_HW_MAX_SERIES_NUM       (LTC6804_CHIP_NUM * LTC6804_CELL_NUM)
#else
#define BMS_HW_MAX_SERIES_NUM       BMS_CELL_SERIES_NUM
#endif

/* ====== BQ76952 参数(I2C AFE, 替代 LTC6804 方案) ======
 * 参考: TI BQ76952 TRM (SLUUX87 / SLUUBY2B)
 * 原生 3~16S, 实际串数由 BMS_CELL_SERIES_NUM 控制(UI 可下发 1~16 切换)
 * 电压/温度寄存器单位为 mV(TI 数据手册 Table 2: Cell Voltage Unit=mV;
 *   TS 引脚须配置为 ADCIN 模式, 0x70/0x72/0x74 才返回 mV, 否则返回 0.1K 温度)
 * 内置被动均衡由子命令 CB_ACTIVE_CELLS(0x0083) 控制, 非直接命令
 * 与 OLED/BQ34Z100 共用 I2C_NUM_0(SDA=GPIO8 / SCL=GPIO9) */
#define BQ76952_I2C_ADDR           0x10        // 7-bit I2C 地址(ADDR 引脚接地)
#define BQ76952_VLSB_MV             1.0f        // 电压分辨率=1 mV(TI 数据手册: VC/TS 寄存器单位均为 mV)
#define BQ76952_CELL_NUM           16          // 单芯片最大支持 16 串
#define BQ76952_ADC_CONV_MS         10         // 电压 ADC 转换时间余量, 单位 ms(实际为已转换值直读)

/* ====== OLED(SH1106) ====== */
#define OLED_WIDTH                  128        // 屏幕宽度, 单位 pixel
#define OLED_HEIGHT                 64         // 屏幕高度, 单位 pixel
#define OLED_I2C_ADDR               0x3C       // I2C 设备地址

/* ====== CAN(MCP2515) ====== */
#define CAN_BAUDRATE                500000     // CAN 波特率 500kbps

/* ====== SD 卡黑匣子 ====== */
#define SD_LOG_PERIOD_NORMAL_MS     1000       // 正常记录周期, 单位 ms
#define SD_LOG_PERIOD_FAULT_MS      100        // 故障高频记录周期, 单位 ms

/* ====== FreeRTOS 任务参数 ====== */
#define TASK_ACQUISITION_PRIO       5
#define TASK_ACQUISITION_STACK      4096
#define TASK_ACQUISITION_PERIOD_MS  100

#define TASK_PROTECTION_PRIO        6
#define TASK_PROTECTION_STACK       4096
#define TASK_PROTECTION_PERIOD_MS   100

#define TASK_SOC_PRIO               4
#define TASK_SOC_STACK              4096
#define TASK_SOC_PERIOD_MS          1000

#define TASK_NN_PRIO                3
#define TASK_NN_STACK               8192
#define TASK_NN_PERIOD_MS           5000

#define TASK_BALANCE_PRIO           3
#define TASK_BALANCE_STACK          2048
#define TASK_BALANCE_PERIOD_MS      10000

#define TASK_COMM_PRIO              2
#define TASK_COMM_STACK             8192   /* 2026-08-08: 4096→8192 修复栈溢出(task_comm 调用 sys_mqtt_report 局部缓冲~2KB) */
#define TASK_COMM_PERIOD_MS         1000

/* 2026-08-11: 设备属性上报间隔(秒). 每 TASK_COMM_PERIOD_MS(1000ms) 累加一次
 *   mqtt_report_cnt, 故计数==秒. 默认 10s => 8640 条/天, 低于华为云 15000 日上限.
 *   最小 7s(12343/天)由固件 set_param 强制 clamp, 保证永不超过 15000/天.
 * 2026-08-18 双速率架构: 华为云备用通道固定 7s(12343条/天, 仍 <15000 上限),
 *   EMQX 主通道由 task_fast_report 以 100ms 高频精简帧独立上报(864000条/天, 自建 broker 无配额). */
#define BMS_REPORT_INTERVAL_SEC     7

/* 2026-08-18: EMQX 主通道高频上报任务(100ms, 仅发 EMQX 第二通道精简帧)
 *   与 task_comm(1s, 华为云+EMQX 全量帧) 并存: 高频帧只发 bms/<id>/fast, QoS0 尽力而为
 * 2026-08-18 修复: 4096 → 10240 栈溢出(实测 rst:0xc RTC_SW_CPU_RST 周期重启)
 *   原因: publish 路径在 wss+TLS 下触发 mbedtls encrypt/write(约2~4KB 栈),
 *         4096 栈在构造 JSON + 调用链组合下溢出; 与 esp-mqtt 任务 10KB 对齐. */
#define TASK_FAST_PRIO              3
#define TASK_FAST_STACK             10240
#define TASK_FAST_PERIOD_MS         100

#define TASK_OLED_PRIO              1
#define TASK_OLED_STACK             4096          // 增加: 菜单渲染用 snprintf, 栈需求增加
#define TASK_OLED_PERIOD_MS         100           // 改为 100ms, 按键响应更灵敏

#define TASK_SD_LOG_PRIO            1
#define TASK_SD_LOG_STACK           4096
#define TASK_SD_LOG_PERIOD_MS       1000

#define TASK_OTA_PRIO               1
#define TASK_OTA_STACK              8192

/* 报警任务(100ms) - 驱动 LED/蜂鸣器分级报警, 必启动 */
#define TASK_ALARM_PRIO             5
#define TASK_ALARM_STACK            4096   /* 2026-09-09: 2048→4096, 实测高水位仅剩 52B, 濒临溢出 */
#define TASK_ALARM_PERIOD_MS        100

#define TASK_WATCHDOG_PRIO          7
#define TASK_WATCHDOG_STACK         2048   /* 2026-09-09: 1024→2048, 实测高水位仅剩 92B(且需跑栈水位采集) */
#define TASK_WATCHDOG_PERIOD_MS     1000

/* ====== SOC 估算(AEKF) ====== */
#define SOC_INVALID                 -1.0f      // SOC 无效哨兵值
#define SOC_INIT_VALUE              0.50f      // 初始 SOC
#define AEKF_FORGETTING_FACTOR      0.95f      // Sage-Husa 遗忘因子
#define AEKF_INNOV_WINDOW           30         // 新息滑动窗口

/* ====== 电池二阶 RC 等效电路模型参数 (app_soc.c 使用) ====== */
#define BATT_TAU1_SEC              100.0f      // RC1 时间常数, 单位 s (极化)
#define BATT_TAU2_SEC              1000.0f     // RC2 时间常数, 单位 s (扩散)
#define BATT_R_NEW_OHM             0.030f      // 全新电池内阻, 30mΩ
#define BATT_R_EOL_OHM             0.060f      // 寿命终止内阻, 60mΩ (2倍)
#define BATT_R0_MIN_OHM            0.005f      // 内阻下限 5mΩ (辨识限幅)
#define BATT_R0_MAX_OHM            0.100f      // 内阻上限 100mΩ (辨识限幅)
#define AEKF_Q_SOC                 1e-5f       // 过程噪声 Q - SOC 对角元
#define AEKF_Q_VRC                 1e-4f       // 过程噪声 Q - V_RC 对角元
#define AEKF_R_MEAS                1e-3f       // 观测噪声 R 初值

/* ====== SOC 算法扩展(2026-08-09, 对标 2025/2026 最新文献) ======
 * 运行时 soc_algo: 0=AEKF 1=纯安时积分 2=OCV查表 3=MCC-EKF(默认, 2025精度0.78%) 4=UKF
 * 网页 battery_type/soc_algo 可下发切换, 多串数自适应 */
/* MCC-EKF(最大相关熵准则): 高斯核带宽 σ(电压量纲, V)
 *   新息 e>σ 时权重 w=exp(-e²/2σ²) 快速下降 -> R_eff=R/w 增大 -> 增益减小,
 *   自动抑制野值/非高斯噪声对 SOC 估计的污染(对标 2025 Energy 自适应核宽 MCCEKF) */
#define MCC_KERNEL_WIDTH_V         0.05f       // 核带宽 σ: 电压量纲, 约为正常新息幅度的 2~3 倍
#define MCC_WEIGHT_MIN             0.05f       // 核权重下限(避免完全拒绝观测导致不收敛)
/* UKF(无迹卡尔曼滤波): sigma 点参数(对标 2026 FOMIUKF/NDO-UKF 的 UKF 基础核)
 *   α 决定 sigma 点散布(0.1~1), β=2 对高斯分布最优, κ 辅助缩放(通常 3-n=0)
 *   ⚠️ float32 下 α 不宜 <0.1: α 过小使 n+λ≈0, 权重巨大, 相加严重抵消
 *   (α=0.1, n=3, κ=0 => λ=-2.97, 权重 |w|≤99, 数值稳定) */
#define UKF_ALPHA                  0.1f        // sigma 点散布系数(float32 数值安全)
#define UKF_BETA                   2.0f        // 先验分布参数(高斯=2)
#define UKF_KAPPA                  0.0f        // 辅助缩放(3 状态取 0)
/* OCV-SOC 三次多项式拟合: OCV = a + b*SOC + c*SOC^2 + d*SOC^3 (单位 V) */
#define OCV_POLY_A                 3.0f        // 常数项 (截止电压附近)
#define OCV_POLY_B                 1.2f        // 一次项
#define OCV_POLY_C                 -0.3f       // 二次项
#define OCV_POLY_D                 0.5f        // 三次项

/* ====== 充电策略(CC-CV) ====== */
#define CHARGE_CC_CURRENT_MA        2000       // 恒流阶段电流, 2A
#define CHARGE_CC_SOC_THRESHOLD     0.80f      // CC->CV 切换点 SOC
#define CHARGE_CV_SOC_THRESHOLD     0.95f      // 充满停止 SOC

/* ====== SD 卡挂载点 ====== */
#define SD_MOUNT_POINT              "/sdcard"

/* ====== WiFi 配置(首次配网后由 NVS 覆盖) ======
 * 安全: 默认不烧录任何 WiFi 口令到固件(BMS_WIFI_DEFAULTS_ENABLED=0),
 *       首次开机因 NVS 无凭据 -> sys_wifi_init_sta() 返回失败 ->
 *       自动进入 AP 配网门户(192.168.4.1), 由用户/现场填写后写入 NVS 永久保存。
 *       生产构建必须保持此默认, 禁止把明文口令烧录进固件(质量评估 09-P1)。
 *       仅开发调试需要时可临时置 1 启用下方默认值(切勿提交到生产分支)。
 * 启动流程:
 *   1. 读 NVS, 若已配网用 NVS 中的 SSID/密码 STA 模式连接
 *   2. 若 NVS 未配置或连接失败超时, 切换 AP 模式进入配网门户
 *   3. 用户访问 192.168.4.1 填写 WiFi/MQTT 参数, 保存到 NVS 后重启 */
#define BMS_WIFI_DEFAULTS_ENABLED   1                       // 1=启用下方开发默认口令; 0=生产安全(强制首次配网)
#if BMS_WIFI_DEFAULTS_ENABLED
#define BMS_WIFI_SSID               "WIFI-A393"            // [开发] 默认 SSID(NVS 优先)
#define BMS_WIFI_PASS               "1234567890"          // [开发] 默认密码(NVS 优先)
#else
#define BMS_WIFI_SSID               ""                     // 生产: 不烧录口令, 强制首次配网
#define BMS_WIFI_PASS               ""
#endif
#define BMS_WIFI_CONNECT_TIMEOUT_MS 10000               // STA 连接超时, 单位 ms
#define BMS_WIFI_MAX_RETRY          3                   // STA 重试次数

/* AP 配网模式参数(首次开机或长按 BOOT 5s 进入) */
#define BMS_AP_SSID                 "BMS_Config"        // AP 热点名称
#define BMS_AP_PASS                 "12345678"                  // AP 密码(空=开放)
#define BMS_AP_CHANNEL              1                   // AP 信道
#define BMS_AP_MAX_CONN             2                   // AP 最大连接数
#define BMS_CONFIG_PORTAL_TIMEOUT_MS 300000             // 配网门户超时, 单位 ms (5 分钟无操作自动重启)
#define BMS_CONFIG_BUTTON_LONG_MS   5000                // 长按 BOOT 多少 ms 强制进入配网模式

/* ====== MQTT 配置(华为云 IoTDA 接入层, 阿里云 ECS 仅作后端) ======
 * 架构: ESP32 → 华为云 IoTDA(MQTT 接入层) → 阿里云 ECS Dashboard(IoTDA REST 轮询)
 * 主题设计(华为云 IoTDA 原生格式):
 *   $oc/devices/{id}/sys/properties/report     ESP32 → 云端, 属性上报
 *   $oc/devices/{id}/sys/commands/#            云端 → ESP32, 命令下发
 *   $oc/devices/{id}/sys/commands/response/{request_id}  ESP32 → 云端, 命令响应
 *   $oc/devices/{id}/sys/events/up             ESP32 → 云端, 事件/故障上报
 * 鉴权方式: ClientID={device_id}_0_0_{timestamp}, Username={device_id},
 *           Password=HMAC-SHA256(设备密钥, 时间戳) */
#define BMS_MQTT_URI                "mqtts://6a3ff62aab.iotda-device.cn-south-4.myhuaweicloud.com:8883"
#define BMS_MQTT_CLIENT_ID          "<IOTDA_DEVICE_ID>_BMS001"  // 设备ID
#define BMS_MQTT_USER               "<IOTDA_DEVICE_ID>_BMS001"  // 用户名(同设备ID)
#define BMS_MQTT_DEVICE_SECRET      "164db82cacf6c49c0109"                       // 设备密钥(华为云 IoTDA 控制台实际密钥, 配网门户可覆盖写入NVS)
#define BMS_MQTT_PASS               ""                                  // 动态生成,此处留空
#define BMS_MQTT_TOPIC_PREFIX       "$oc/devices/"                      // Topic前缀(IoTDA 格式)
#define BMS_MQTT_TOPIC_DATA         "properties/report"                 // 属性上报
#define BMS_MQTT_TOPIC_FAULT        "events/up"                         // 事件/故障上报
#define BMS_MQTT_TOPIC_CMD          "commands/#"                        // 命令订阅
#define BMS_MQTT_TOPIC_CMD_RESP     "commands/response"                 // 命令响应前缀
#define BMS_MQTT_TOPIC_INFO         "properties/report"                 // 设备信息也走属性上报
#define BMS_MQTT_SERVICE_ID         "BMS"                               // 服务ID
#define BMS_MQTT_QOS                1                                   // QoS 级别(1=至少一次)
/* 2026-08-10 工业标准: keepalive 120→60s
 *   华为云 IoTDA 服务端空闲超时约为 120~180s(网络设备/NAT 静默丢包时更早),
 *   120s 心跳在运营商 NAT/防火墙回收空闲连接后, 设备要等 2 个 keepalive 周期
 *   才能发现断线(MQTT 协议: 服务端超时 = 1.5×keepalive). 60s 心跳:
 *   - 断线感知 ≤90s, 网页离线提示更快(配合后端 60s 陈旧判定)
 *   - 每连接仅多 1 条 PINGREQ/分钟, 流量可忽略
 *   - 与 15s 数据上报 + 60s 数据陈旧阈值形成"上报/心跳/判定"三级时间链
 *   注: IoTDA 空闲超时 120~180s, 60s 心跳同样适用 */
#define BMS_MQTT_KEEPALIVE_SEC      60                                  // 心跳间隔, 单位 s

/* ====== 自建 Mosquitto 双通道配置(2026-08-16 新增, 与华为云双发并存) ======
 * 架构: ESP32 将同一份属性 JSON 双发 →
 *   ① 华为云 IoTDA(影子/规则/告警, 主)
 *   ② 自建 Mosquitto(实时推送/手机App直连, 备)
 * 主题: bms/<device_id>/telemetry | event | status | cmd/ack (见《迁自建服务器实施步骤》3.1)
 * 鉴权: 静态用户名/密码(与华为云 HMAC 动态密码不同, 属第二通道) */
#define BMS_MQTT2_URI            "wss://emqx.bms0605.dpdns.org/mqtt"     // 自建 EMQX(经 cloudflared HTTPS 隧道, 免费版隧道不支持 TCP 直连; WSS 443)
#define BMS_MQTT2_CLIENT_ID      "bms01"                                // 自建 broker 客户端ID
#define BMS_MQTT2_USER           "bms01"                                // 自建 broker 用户名(2026-09-09 专属账号, 替代共享 student, ACL 仅限 bms/bms01/#)
#define BMS_MQTT2_PASS           "4FJvVic2vN6f7PwVVMcnkMu7"             // 自建 broker 密码(服务器 /root/.bms01_pw.txt)
#define BMS_MQTT2_TOPIC_PREFIX   "bms/"                                 // 自建 topic 前缀
#define BMS_MQTT2_TOPIC_TELE     "telemetry"                            // 遥测(复用华为云 services 包裹 JSON)
#define BMS_MQTT2_TOPIC_EVENT    "event"                                // 事件/故障
#define BMS_MQTT2_TOPIC_STATUS   "status"                               // 在线状态(LWT retained)
#define BMS_MQTT2_TOPIC_CMD_ACK  "cmd/ack"                              // 命令回执

/* ====== OTA 固件升级配置 ======
 * 工作流程:
 *   1. ESP32 定期访问 version_url 查询最新版本号(JSON: {"version":"1.0.1","url":"..."})
 *   2. 若版本号高于当前, 下载 firmware_url 的 bin 文件
 *   3. 写入 OTA 备用分区, 校验 SHA256, 标记 pending, 重启
 *   4. 新固件启动后 app_ota_init() 标记 valid, 否则回滚
 * 部署方法:
 *   - 云部署(2026-08-16): 后端已迁阿里云 ECS <YOUR_ECS_IP> (docker 三件套),
 *     cloudflared 隧道 bms0605.dpdns.org 也已部署到 ECS, 域名固定 HTTPS 提供 OTA
 *   - 也可以把 bms.bin 和 version.json 上传到 GitHub Release / 任意 HTTPS 服务器
 *   - 本地调试: http://<电脑IP>:5000/api/ota/version.json (Dashboard 本地运行时) */
#define BMS_OTA_VERSION_URL         "https://bms0605.dpdns.org/api/ota/version.json"    /* 云部署: ECS 隧道 OTA 服务(域名固定) */
#define BMS_OTA_FIRMWARE_URL        "https://bms0605.dpdns.org/api/ota/firmware.bin"     /* 同上, 实际由 version.json 覆盖 */
#define BMS_OTA_CHECK_PERIOD_MS     (12 * 60 * 60 * 1000)  /* 检查周期 12 小时(避免404刷屏, 开发期可手动 ota_check) */
#define BMS_OTA_TIMEOUT_MS          (5 * 60 * 1000)        /* 单次下载超时 5 分钟 */
#define BMS_OTA_BUFFER_SIZE         4096                   /* 下载缓冲区大小 */
#define BMS_FIRMWARE_VERSION        "1.0.4"                /* 当前固件版本号(每次发布新固件改大, 触发ESP32 OTA; 1.0.4=2026-09-15 TS2飞线唤醒G15) */

/* 版本可追溯(2026-09-14 高可靠加固): 完整构建指纹 = 版本 + 编译日期时间
 * esp_app_desc_t 由构建系统每次构建刷新(ccache 命中也更新),
 * 运行时取 esp_app_get_description() 组装; 日志/OTA/get_info 统一携带,
 * 现场任何一台设备都能精确对到"哪次编译出的固件" */
#define BMS_BUILD_FINGERPRINT_FMT   "%s|%s %s"             /* version|date time */

/* 重新烧录检测(2026-08-13 新增):
 * =1 串口重新烧录不同固件后, 启动时自动清除 WiFi 配置并进入 AP 配网模式
 *    (判定依据: 运行分区非 OTA 状态 && 编译指纹[版本+日期+时间]与 NVS 记录不同)
 * =0 关闭(与旧行为一致: 烧录后沿用 NVS 旧配置)
 * 注: OTA 升级(esp_https_ota)后首启为 PENDING_VERIFY, 不会触发清配置 */
#define BMS_REPROVISION_ON_REFLASH  1

#endif // BMS_CONFIG_H
