# -*- coding: utf-8 -*-
"""
服务端账号管理 CLI —— 配合"登录模型 B: 独立账号"使用.

当前 Web UI 暂无"管理员建账号"页面, 团队多人独立账号先用本脚本在服务端批量建/管.
用法(在 backend/ 目录, 用项目 venv 的 python 运行):

  python manage_users.py add    <用户名> <密码(>=8位)> <role:admin|operator|viewer> [显示名]
  python manage_users.py list
  python manage_users.py reset  <用户名> <新密码(>=8位)>
  python manage_users.py delete <用户名>

role 权限:
  admin    —— 全权(含下发指令、改系统设置、管账号)
  operator —— 可查看 + 下发控制指令, 不能改系统设置/管账号
  viewer   —— 只读

注意: 首次启动 app.py 会自动建 admin/admin123(见 ensure_default_admin);
       一旦用户表非空, 旧"共享密码(DASHBOARD_PASSWORD)回退登录"自动失效,
       所以建完成员账号后无需手动关闭共享密码。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from database import (create_user, list_users, reset_password, delete_user,
                      ensure_default_admin, init_db)


def _print_users():
    ensure_default_admin()  # 保证至少有一个管理员可登录
    rows = list_users()
    if not rows:
        print("(无账号)")
        return
    print("%-4s %-16s %-10s %-16s %s" % ("ID", "用户名", "角色", "显示名", "最后登录"))
    for u in rows:
        last = u.get("last_login") or 0
        last_s = ""
        if last:
            import time as _t
            last_s = _t.strftime("%Y-%m-%d %H:%M", _t.localtime(last))
        print("%-4d %-16s %-10s %-16s %s" %
              (u["id"], u["username"], u["role"], u.get("display_name") or "", last_s))


def main():
    args = sys.argv[1:]
    init_db()  # 确保 users 等表已建(幂等)
    if not args:
        print(__doc__)
        return
    cmd = args[0]

    if cmd == "add":
        # add <user> <pw> <role> [name]
        if len(args) < 4:
            print("用法: add <用户名> <密码(>=8位)> <role:admin|operator|viewer> [显示名]")
            return
        user, pw, role = args[1], args[2], args[3]
        name = args[4] if len(args) > 4 else ""
        if role not in ("admin", "operator", "viewer"):
            print("role 必须是 admin | operator | viewer")
            return
        if len(pw) < 8:
            print("密码至少 8 位")
            return
        if create_user(user, pw, role=role, display_name=name):
            print("已创建账号 %s (role=%s)" % (user, role))
        else:
            print("账号 %s 已存在, 未重复创建" % user)

    elif cmd == "list":
        _print_users()

    elif cmd == "reset":
        if len(args) < 3:
            print("用法: reset <用户名> <新密码(>=8位)>")
            return
        if len(args[2]) < 8:
            print("新密码至少 8 位")
            return
        ok = reset_password(args[1], args[2])
        print("已重置 %s 的密码" % args[1] if ok else "用户 %s 不存在" % args[1])

    elif cmd == "delete":
        if len(args) < 2:
            print("用法: delete <用户名>")
            return
        ok, msg = delete_user(args[1])
        print(msg)

    else:
        print("未知命令: %s\n" % cmd)
        print(__doc__)


if __name__ == "__main__":
    main()
