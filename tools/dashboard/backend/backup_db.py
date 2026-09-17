# -*- coding: utf-8 -*-
"""
BMS 数据库自动备份(2026-08-09 新增)
============================================
功能: 压缩备份 bms_history.db 到 backups/ 目录, 保留最近 N 份
用法:
  手动:   python backup_db.py
  自动:   在 app.py 中由后台线程每日调用(见 backup_db_now)
配置(dashboard.env):
  BMS_DB_BACKUP_ENABLED=1      # 1=启用每日自动备份(默认1) 0=关闭
  BMS_DB_BACKUP_KEEP=14        # 保留最近多少份(默认14天)
说明:
  - 使用 sqlite3 backup API(VACUUM INTO 等价)在线备份, 不影响运行中写入
  - 备份文件压缩为 .gz, 命名 bms_history_YYYYMMDD_HHMMSS.db.gz
  - 自动清理超过保留份数的旧备份
"""
import os
import time
import gzip
import glob
import shutil
import datetime
import sqlite3

_DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bms_history.db")
_BACKUP_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "backups")


def backup_db_now(db_path=None, backup_dir=None):
    """执行一次备份, 返回备份文件路径; 失败返回 None
    db_path:   数据库路径(默认 bms_history.db)
    backup_dir:备份目录(默认 backups/)"""
    db_path = db_path or _DB_PATH
    backup_dir = backup_dir or _BACKUP_DIR
    if not os.path.exists(db_path):
        return None

    os.makedirs(backup_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    tmp_file = os.path.join(backup_dir, "bms_history_%s.tmp" % stamp)
    gz_file = os.path.join(backup_dir, "bms_history_%s.db.gz" % stamp)

    try:
        # 1. sqlite3 backup API: 在线一致性备份到临时文件
        src = sqlite3.connect(db_path)
        dst = sqlite3.connect(tmp_file)
        try:
            src.backup(dst)
        finally:
            dst.close()
            src.close()
        # 2. 压缩
        with open(tmp_file, "rb") as fin, gzip.open(gz_file, "wb", compresslevel=6) as fout:
            shutil.copyfileobj(fin, fout)
        os.remove(tmp_file)

        # 3. 清理旧备份(按保留份数)
        keep = int(os.environ.get("BMS_DB_BACKUP_KEEP", "14") or 14)
        old = sorted(glob.glob(os.path.join(backup_dir, "bms_history_*.db.gz")))
        while len(old) > keep:
            os.remove(old[0])
            old = old[1:]

        size = os.path.getsize(gz_file)
        print("[BACKUP] 已备份: %s (%.1f KB)" % (gz_file, size / 1024.0))
        return gz_file
    except Exception as e:
        print("[BACKUP] 备份失败: %s" % e)
        try:
            if os.path.exists(tmp_file):
                os.remove(tmp_file)
        except Exception:
            pass
        return None


def _auto_backup_once():
    """供 app.py 后台线程调用的每日备份(带配置开关)"""
    enabled = os.environ.get("BMS_DB_BACKUP_ENABLED", "1") not in ("0", "false", "False")
    if not enabled:
        return None
    return backup_db_now()


if __name__ == "__main__":
    r = backup_db_now()
    print("结果:", "OK" if r else "FAILED(数据库不存在或异常)")
