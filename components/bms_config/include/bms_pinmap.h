/**
 * @file    bms_pinmap.h
 * @brief   BMS 引脚映射集中定义 (ESP32-S3 最小系统板, 仿 LibreSolar BMS-C1 外设布局)
 * @author  BMS Team
 * @date    2026-08
 * @note    主控 = ESP32-S3 最小系统板, 可用引脚 G0~G21 + G35~G48 (G22~G34 未引出)
 *          S3 约束: strapping=G0/G3/G45/G46, USB-JTAG=G19/G20, UART0=G43/G44
 *          外设按 BMS-C1 布局: BQ76952(I2C) + TWAI CAN(SN65HVD230) + RS485(SN65HVD75)
 *          + 红/绿双 LED, 无 MCP2515/继电器/INA181/OLED/SD/蜂鸣器/BQ34Z100
 *          修改引脚只需改此处, 全工程自动生效
 */
#ifndef BMS_PINMAP_H
#define BMS_PINMAP_H

/* ====== I2C0 (BQ76952 AFE; 与 BMS-C1 v0.4 一致, 同原工程) ====== */
#define PIN_I2C_SDA                 8          // I2C 数据线 (ESP32-S3 GPIO8)
#define PIN_I2C_SCL                 9          // I2C 时钟线 (ESP32-S3 GPIO9)

/* ====== BQ76952 AFE ======
 * WAKE(TS2 唤醒): 2026-09-15 硬件飞线 — GPIO15(U5 第14脚, P4 原理图 NC 空闲)
 * 开漏拉低 R25 上端/TS2 焊盘 50ms 复刻按键下降沿唤醒 SHUTDOWN;
 * 原 C1 未接 WAKE 依赖 I2C 自唤醒(-1 禁用), 现飞线后改为 15.
 * ALERT: BQ76952 事件中断输出(开漏, C1 接 GPIO2 高有效; S3 上 GPIO2 被 LED 绿占用,
 *        故接空闲 GPIO12, 上拉 10k, 高电平=有事件; 软件以中断+轮询双保险) */
#define PIN_BQ76952_WAKE           15          // 唤醒引脚(飞线到 TS2 焊盘, 开漏拉低); -1=不接
#define PIN_BQ76952_ALERT          12          // ALERT 事件引脚(高有效; 2026-08-25 新增, C1 为 GPIO2)

/* ====== TWAI (CAN, SN65HVD230 收发器; 与 BMS-C1 一致) ======
 * 替代原 MCP2515 SPI 方案, 用 ESP32-S3 内置 TWAI 控制器 */
#define PIN_CAN_TX                  5          // TWAI TX (接收发器 D)
#define PIN_CAN_RX                  4          // TWAI RX (接收发器 R)
#define PIN_CAN_STB                48          // 收发器待机控制 (STB, 低=正常)

/* ====== RS485 (SN65HVD75, Modbus RTU; UART1) ======
 * 数据线用 UART1 原生直连引脚: TX=GPIO17(U1TXD) RX=GPIO18(U1RXD)
 * (依据 ESP32-S3-WROOM-1 数据手册引脚定义, 2026-08-25 用户提供 PDF 确认;
 *  原 GPIO16/17 方案改为 17/18 —— GPIO16 在手册中标注为 U0CTS, 非 UART1 原生脚)
 * DE/RE 短接共用一个 GPIO47: 高=发送, 低=接收使能
 * (DE=active high, ~RE=active low, 逻辑天然匹配) */
#define PIN_RS485_TX               14         // RS485 发送 (UART1, GPIO matrix)
#define PIN_RS485_RX               21         // RS485 接收 (UART1, GPIO matrix)
#define PIN_RS485_DE               47         // 方向控制 DE/RE 短接 (高=发送, 低=接收)
#define PIN_RS485_RE               47         // 与 DE 同一引脚 (短接一起)

/* ====== LED 三灯状态显示 (2026-08-26 扩展为三灯) ======
 * GPIO1=红, GPIO2=绿, GPIO38=板载白色RGB(共阳, GPIO38=低亮/高灭)
 * 状态编码:
 *   关机      → 全灭
 *   正常      → 绿灯亮
 *   预警      → 白灯慢闪(1Hz)
 *   保护      → 红灯快闪(5Hz) + 白灯快闪
 *   严重      → 红灯常亮 + 白灯常亮
 *   充电中    → 白灯恒亮
 *   WiFi连接中 → 白灯快闪(3Hz) */
#define PIN_LED_RED                 1          // 红色状态灯 (保护/严重)
#define PIN_LED_GREEN               2          // 绿色状态灯 (正常)
#define PIN_LED_WHITE               38         // 板载白色 RGB (共阳, 低=亮, 高=灭)

/* ====== 按钮(用户输入) ======
 * BOOT 键(GPIO0, 板上自带): 短按翻页 / 长按 3s 复位故障 / 双击切换页面
 * 独立配网通道: 长按 5s 清配置重启进 AP */
#define PIN_BUTTON_USER             0

/* ====== 蜂鸣器(有源蜂鸣器, 高电平有效, 2026-08-25 新增) ======
 * GPIO11: 空闲引脚, 远离功能脚; 需经 NPN 三极管(如 S8050)驱动, 蜂鸣器接 3.3V/5V */
#define PIN_BUZZER                 11

/* ====== SW_PWR 电源键(2026-08-25 新增, 仿 C1: 长按3s关闭功率输出) ======
 * C1 中 SW_PWR=GPIO3(高有效, 按下=高); S3 上 GPIO3 是 strapping 不能用,
 * 改用空闲 GPIO13(普通 GPIO, 无 FSPI/Flash 占用, 可中断输入);
 * 按键一端接 GPIO13、一端接 3.3V(高有效, 按下=高), 软件长按 3s 关 FET 功率 */
#define PIN_SW_PWR                 13         // 电源键 (高有效, 仿 C1; 长按3s关功率)

/* ====== 预留/约束说明 ======
 * G19/G20  = USB-Serial-JTAG (COM6 烧录/日志), 不可占用
 * G43/G44  = UART0 RX0/TX0 (调试串口), 预留
 * G3/G45/G46 = strapping 引脚, 避免作输出
 * 其余空闲: G7, G11~G15, G18, G21, G35~G42, G47, G48 */

#endif // BMS_PINMAP_H
