# -*- coding: utf-8 -*-
# 自定义 MCP 工具系统（游戏侧）。
# 提供 @mcp_tool 装饰器、注册表、用户目录发现、返回契约转换，以及供 IPCSystem 注册的
# list_custom_tools / call_tool / rescan_custom_tools JSON handler。
# Py2.7 兼容：禁止 f-string / type hints / print() 函数式以外用法。
import os
import json
import traceback

from .IPCSystem import (
    _JSON_SAFE_VALUE,
    _UNICODE_TYPE,
    CALL_ON_SERVER_THREAD,
    CALL_ON_CLIENT_THREAD,
)
from .Config import TARGET_MOD_DIRS


# ── 注册表 ──────────────────────────────────────────────────────────
# name -> {"name","description","params"(list),"side","func"}
_CUSTOM_TOOLS = {}
_BUILTIN_NAMES = set()   # 内置工具名（重扫用户目录时保留）
_USER_NAMES = set()      # 用户目录发现的工具名（重扫前清理）
_LAST_SCAN_ERRORS = []   # [{"file","error"}]，最近一次用户目录扫描的错误
_BUILTIN_LOADED = False


def mcp_tool(name, description="", params=None, side="server"):
    # type: (str, str, list | None, str) -> callable
    """装饰普通函数为自定义 MCP 工具。原样返回函数，仅记录元数据。

    params: [{"name","type"("string"/"number"/"boolean"/"array"/"object"),
              "description","required"(bool)}, ...]
    side:   "server" | "client"
    """
    if side not in ("server", "client"):
        side = "server"
    paramList = params if isinstance(params, list) else []

    def _decorator(func):
        _CUSTOM_TOOLS[name] = {
            "name": name,
            "description": description or "",
            "params": paramList,
            "side": side,
            "func": func,
        }
        return func

    return _decorator


# ── 返回契约（§5.4）：最终交给 AI 必为字符串，永不抛 ──────────────────
def _CONVERT_RESULT(value):
    # type: (object) -> str
    try:
        # 1. str / unicode 原样透传（仅保证 UTF-8）
        if isinstance(value, _UNICODE_TYPE) and not isinstance(value, str):
            return value.encode("utf-8")
        if isinstance(value, str):
            # 含非 ASCII 时按 UTF-8 规整（已是字节串则原样）
            return value
        # 2. dict / list / tuple → 结构化 JSON 文本
        if isinstance(value, (dict, list, tuple)):
            return json.dumps(_JSON_SAFE_VALUE(value), ensure_ascii=False, indent=2)
        # 3. None / 数值 / bool → str()
        if value is None:
            return "None"
        if isinstance(value, bool):
            return str(value)
        try:
            _long = long  # noqa: F821 (Py2)
        except NameError:
            _long = int
        if isinstance(value, (int, _long, float)):
            return str(value)
        # 4. 其它/自定义对象 → _JSON_SAFE_VALUE 再 dumps
        return json.dumps(_JSON_SAFE_VALUE(value), ensure_ascii=False, indent=2)
    except Exception:
        # 5. 兜底 repr，仍不抛
        try:
            return repr(value)
        except Exception:
            return "<convert failed>"


# ── 发现 ────────────────────────────────────────────────────────────
def _LOAD_BUILTIN():
    """导入内置工具模块，触发其 @mcp_tool 装饰器注册。"""
    global _BUILTIN_LOADED
    try:
        from . import McpToolsBuiltin  # noqa: F401  (导入即注册)
        _BUILTIN_LOADED = True
        for toolName in _CUSTOM_TOOLS.keys():
            _BUILTIN_NAMES.add(toolName)
    except Exception:
        traceback.print_exc()


def _EXEC_USER_FILE(filePath):
    """读取并执行单个用户工具文件；注入 mcp_tool 装饰器到其命名空间，规避 import 路径问题。"""
    f = open(filePath, "rb")
    try:
        source = f.read()
    finally:
        f.close()
    namespace = {
        "__name__": "mcp_user_tool",
        "__file__": filePath,
        "mcp_tool": mcp_tool,
    }
    code = compile(source, filePath, "exec")
    exec(code, namespace, namespace)


def _SCAN_ONE_DIR(toolsDir):
    # 扫描单个目录下的 *.py（跳过 __ 开头），exec 注入 mcp_tool。错误记入 _LAST_SCAN_ERRORS。
    if not toolsDir or not os.path.isdir(toolsDir):
        return
    for fileName in os.listdir(toolsDir):
        if not fileName.endswith(".py") or fileName.startswith("__"):
            continue
        filePath = os.path.join(toolsDir, fileName)
        try:
            _EXEC_USER_FILE(filePath)
        except Exception:
            _LAST_SCAN_ERRORS.append({
                "file": filePath,
                "error": traceback.format_exc(),
            })


def _COLLECT_TOOL_DIRS():
    # 收集待扫描目录：各 TARGET_MOD_DIR/mcp_tools + 全局目录(MCDEV_GLOBAL_MCP_TOOLS,由 mcdk 注入)。
    dirs = []
    for rootDir in TARGET_MOD_DIRS:
        dirs.append(os.path.join(rootDir, "mcp_tools"))
    globalDir = os.getenv("MCDEV_GLOBAL_MCP_TOOLS")
    if globalDir:
        dirs.append(globalDir)
    # 去重（规整后比较），保持顺序
    seen = set()
    result = []
    for d in dirs:
        try:
            key = os.path.normcase(os.path.abspath(d))
        except Exception:
            key = d
        if key in seen:
            continue
        seen.add(key)
        result.append(d)
    return result


def SCAN_USER_TOOLS():
    """扫描所有工具目录(项目 mcp_tools/ + 全局目录)，发现并注册用户自定义工具。

    重扫前清掉上次用户项；内置项保留。返回错误列表。
    """
    global _LAST_SCAN_ERRORS
    # 清掉上次用户注册项
    for toolName in list(_USER_NAMES):
        if toolName not in _BUILTIN_NAMES:
            _CUSTOM_TOOLS.pop(toolName, None)
    _USER_NAMES.clear()
    _LAST_SCAN_ERRORS = []

    before = set(_CUSTOM_TOOLS.keys())
    for toolsDir in _COLLECT_TOOL_DIRS():
        try:
            _SCAN_ONE_DIR(toolsDir)
        except Exception:
            _LAST_SCAN_ERRORS.append({
                "file": str(toolsDir),
                "error": traceback.format_exc(),
            })

    after = set(_CUSTOM_TOOLS.keys())
    for toolName in (after - before):
        _USER_NAMES.add(toolName)
    return _LAST_SCAN_ERRORS


def DISCOVER_ALL():
    """完整发现：导入内置 + 首扫用户目录。供 init 调用。"""
    if not _BUILTIN_LOADED:
        _LOAD_BUILTIN()
    SCAN_USER_TOOLS()


def _TOOL_LIST_PAYLOAD():
    tools = []
    for meta in _CUSTOM_TOOLS.values():
        tools.append({
            "name": meta["name"],
            "description": meta["description"],
            "params": meta["params"],
            "side": meta["side"],
            "builtin": meta["name"] in _BUILTIN_NAMES,  # 内置工具(如 health_check)始终注册
        })
    return {"tools": tools, "errors": _LAST_SCAN_ERRORS}


# ── IPC JSON handlers ───────────────────────────────────────────────
def JSON_LIST_CUSTOM_TOOLS(params, callback):
    try:
        if not _BUILTIN_LOADED:
            _LOAD_BUILTIN()
        # 每次拉取都重扫用户目录，保证 mcdk 侧 doSync 反映最新的增删工具文件。
        SCAN_USER_TOOLS()
        callback(_TOOL_LIST_PAYLOAD())
    except Exception as e:
        callback(None, False, {
            "code": "list_custom_tools_error",
            "message": str(e),
            "traceback": traceback.format_exc(),
        })


def JSON_RESCAN_CUSTOM_TOOLS(params, callback):
    try:
        if not _BUILTIN_LOADED:
            _LOAD_BUILTIN()
        SCAN_USER_TOOLS()
        callback(_TOOL_LIST_PAYLOAD())
    except Exception as e:
        callback(None, False, {
            "code": "rescan_custom_tools_error",
            "message": str(e),
            "traceback": traceback.format_exc(),
        })


def JSON_CALL_TOOL(params, callback):
    name = params.get("name", "")
    toolParams = params.get("params", {})
    if not isinstance(toolParams, dict):
        toolParams = {}
    timeout = params.get("timeout", 10.0)
    try:
        timeout = float(timeout)
    except Exception:
        timeout = 10.0

    meta = _CUSTOM_TOOLS.get(name)
    if meta is None:
        callback(None, False, {
            "code": "tool_not_found",
            "message": "Custom tool not found: " + str(name),
        })
        return

    func = meta["func"]
    side = meta["side"]

    # 把 unicode key 转成 str 以便 **kwargs（Py2 不接受 unicode 关键字参数）
    kwargs = {}
    for k, v in toolParams.items():
        try:
            key = k.encode("utf-8") if isinstance(k, _UNICODE_TYPE) and not isinstance(k, str) else str(k)
        except Exception:
            key = str(k)
        kwargs[key] = v

    def _INVOKE():
        return _CONVERT_RESULT(func(**kwargs))

    try:
        if side == "client":
            resultStr = CALL_ON_CLIENT_THREAD(_INVOKE, timeout)
        else:
            resultStr = CALL_ON_SERVER_THREAD(_INVOKE, timeout)
        callback({"return_string": resultStr, "name": name, "side": side})
    except Exception as e:
        callback(None, False, {
            "code": "call_tool_error",
            "message": str(e),
            "traceback": traceback.format_exc(),
        })
