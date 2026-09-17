# -*- coding: utf-8 -*-
"""
数值解析安全 helper
====================
根治代码复审 Round3/4 发现的 F1 (_shadow_to_payload 元素级 float 转换崩溃)
与 F1-ext (database.insert_data 元素级 int 转换崩溃) 问题:

  - 华为云物模型回传的 cells/temps 可能是畸形字符串(如 "", "N/A", "null"),
    直接 float()/int() 会抛 ValueError, 冒泡后把设备误判离线 / 静默丢数据点。
  - 这里统一提供「坏值兜底、绝不抛异常」的解析函数, 供 iotda_client、database、
    mqtt_client 三处共用, 一次性消除该类隐患。

设计要点:
  - safe_int / safe_float: 单值解析, 失败/None/空串返回 default(默认 0 / 0.0)。
  - safe_int_list / safe_float_list: 数组解析。
      default_if_bad=None -> 丢弃坏元素(保留有效数据)
      default_if_bad=<x>  -> 坏元素兜底为 x(保持数组长度, 用于 cells 串数对齐)
  - 非 list/tuple 输入一律返回 [] (不抛异常)。
"""
# 内部哨兵: 标记「坏元素应被丢弃」
_SKIP = object()


def safe_int(value, default=0):
    """把任意值安全转 int; 失败/None/空串返回 default。"""
    try:
        if value is None:
            return default
        if isinstance(value, str):
            value = value.strip()
            if value == "":
                return default
        return int(float(value))
    except (ValueError, TypeError):
        return default


def safe_float(value, default=0.0):
    """把任意值安全转 float; 失败/None/空串返回 default。"""
    try:
        if value is None:
            return default
        if isinstance(value, str):
            value = value.strip()
            if value == "":
                return default
        return float(value)
    except (ValueError, TypeError):
        return default


def _parse_int(v, default_if_bad):
    """解析单个元素; 失败返回 _SKIP(当 default_if_bad 为 None)或兜底值。"""
    try:
        if v is None:
            return _bad(default_if_bad)
        if isinstance(v, str):
            s = v.strip()
            if s == "":
                return _bad(default_if_bad)
        return int(float(v))
    except (ValueError, TypeError):
        return _bad(default_if_bad)


def _parse_float(v, default_if_bad):
    """解析单个元素; 失败返回 _SKIP(当 default_if_bad 为 None)或兜底值。"""
    try:
        if v is None:
            return _bad(default_if_bad)
        if isinstance(v, str):
            s = v.strip()
            if s == "":
                return _bad(default_if_bad)
        return float(v)
    except (ValueError, TypeError):
        return _bad(default_if_bad)


def _bad(default_if_bad):
    return _SKIP if default_if_bad is None else default_if_bad


def safe_int_list(values, default_if_bad=None):
    """把可迭代对象元素安全转 int。

    default_if_bad=None -> 坏元素直接丢弃;
    default_if_bad=<x>  -> 坏元素兜底为 x(保持数组长度, 用于 cells 串数对齐)。
    非 list/tuple 输入返回 []。
    """
    if not isinstance(values, (list, tuple)):
        return []
    out = []
    for v in values:
        val = _parse_int(v, default_if_bad)
        if val is _SKIP:
            continue
        out.append(val)
    return out


def safe_float_list(values, default_if_bad=None):
    """把可迭代对象元素安全转 float。语义同 safe_int_list。"""
    if not isinstance(values, (list, tuple)):
        return []
    out = []
    for v in values:
        val = _parse_float(v, default_if_bad)
        if val is _SKIP:
            continue
        out.append(val)
    return out
