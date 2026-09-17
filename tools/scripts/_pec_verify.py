"""
bsp_ltc6804.c PEC 回归校验
- 从实际 C 文件抽取 s_pec15_table
- 用表驱动 pec15 (init 0x10, 末位 <<1) 计算各命令 16 位 PEC
- 对照 LTC6804 原厂权威值 (datasheet 写序列 / DC1907 分析仪抓包 / 可运行示例)
运行: python _pec_verify.py
"""
import re, sys

SRC = "components/bms_bsp/bsp_ltc6804.c"

def extract_table(path):
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"s_pec15_table\[256\]\s*=\s*\{([^}]+)\}", txt, re.S)
    if not m:
        raise SystemExit("找不到 s_pec15_table")
    toks = re.findall(r"0x[0-9a-fA-F]+", m.group(1))
    vals = [int(t, 16) for t in toks]
    assert len(vals) == 256, "表项应为 256, 实际 %d" % len(vals)
    return vals

def pec16(T, data):
    p = 0x0010
    for b in data:
        idx = ((p >> 7) ^ b) & 0xFF
        p = ((p << 8) ^ T[idx]) & 0xFFFF
    return (p << 1) & 0xFFFF

T = extract_table(SRC)

# 权威值: (命令名, 命令字, 期望 16 位 PEC)  —— 来自 LTC6804 原厂资料
EXPECT = {
    "WRCFG":  (0x0001, 0x3D6E),
    "RDCFG":  (0x0002, 0x2B0A),
    "RDCVA":  (0x0004, None),   # 仅打印, 不强制(权威示例未列)
    "RDCVB":  (0x0006, None),
    "RDCVC":  (0x0008, None),
    "RDCVD":  (0x000A, None),
    "RDAUXA": (0x000C, None),
    "RDAUXB": (0x000E, None),
    "STCVAD": (0x0010, None),
    "STCVDC": (0x0011, None),
}

ok = True
print("命令    命令字   计算PEC   期望PEC   结果")
for name, (cmd, exp) in EXPECT.items():
    d = [(cmd >> 8) & 0xFF, cmd & 0xFF]
    got = pec16(T, d)
    if exp is None:
        print("%-7s 0x%04X  0x%04X   -         (未强制)" % (name, cmd, got))
    else:
        good = (got == exp)
        ok = ok and good
        print("%-7s 0x%04X  0x%04X   0x%04X    %s" % (name, cmd, got, exp, "OK" if good else "FAIL ***"))

if not ok:
    print("\nFAIL: 与 LTC6804 原厂权威 PEC 不一致, 驱动仍不可用")
    sys.exit(1)
print("\nPASS: WRCFG/RDCFG 命中原厂权威 PEC, 表与 <<1 修复正确")
