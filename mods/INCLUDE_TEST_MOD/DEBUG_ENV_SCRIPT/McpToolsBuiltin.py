# -*- coding: utf-8 -*-
# 内置自定义工具：仅保留一个健康检查,用于确认自定义 MCP 工具链路可用。
# 其它工具请由开发者按需在 mcp_tools/ 目录自定义。
# 现成示例(获取实体/玩家/坐标/顶层UI等)见仓库文档：docs/examples/mcp_tools_example.py
import time

from .McpTools import mcp_tool, _CUSTOM_TOOLS
from . import IPCSystem


@mcp_tool(
    name="health_check",
    description=(
        "自定义 MCP 工具系统健康检查：确认游戏侧可达,返回服务端/客户端就绪状态、"
        "已注册自定义工具数与时间戳。用于验证自定义工具链路是否正常。"
    ),
    params=[],
    side="server",
)
def health_check():
    return {
        "ok": True,
        "server_ready": IPCSystem._SR_GAME_COMP is not None,
        "client_ready": IPCSystem._CL_GAME_COMP is not None,
        "tool_count": len(_CUSTOM_TOOLS),
        "registered_tools": sorted(_CUSTOM_TOOLS.keys()),
        "timestamp": time.time(),
    }
