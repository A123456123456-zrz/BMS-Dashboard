# -*- coding: utf-8 -*-
"""从 mqtt2_globalsign_root.pem 生成正确的 C 字符串头文件"""
import os

base = os.path.dirname(os.path.abspath(__file__))
pem_path = os.path.join(base, "mqtt2_globalsign_root.pem")
out_path = os.path.join(base, "mqtt2_ca_globalsign.h")

with open(pem_path, "r", encoding="utf-8") as f:
    lines = [l.rstrip("\r\n") for l in f if l.strip()]

out = []
out.append("/* Auto-generated: GlobalSign Root CA (cloudflared cert chain trust anchor) */")
out.append("#pragma once")
out.append("static const char mqtt2_ca_globalsign_pem[] =")
for l in lines:
    out.append('    "%s\\n"' % l)
out.append('    "";')
out.append("")

with open(out_path, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(out) + "\n")

print("written:", out_path, "lines:", len(out))
# 自检: 重建 PEM 并让 openssl 解析
import subprocess
pem_rebuilt = "".join(l + "\n" for l in lines)
tmp = os.path.join(base, "mqtt2_ca_check.pem")
with open(tmp, "w", encoding="utf-8") as f:
    f.write(pem_rebuilt)
r = subprocess.run(["openssl", "x509", "-in", tmp, "-noout", "-subject"],
                   capture_output=True, text=True)
print("openssl verify:", r.stdout.strip() or r.stderr.strip())
os.unlink(tmp)
