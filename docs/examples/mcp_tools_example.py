# -*- coding: utf-8 -*-
# 自定义 MCP 工具示例集。
#
# 用法：把本文件(或其中需要的函数)复制到你的工具目录,即可作为一等 MCP 工具使用:
#   - 项目级:  <你的Mod>/mcp_tools/example.py
#   - 全局:    %USERPROFILE%/.mcdk/mcp_tools/example.py
# 复制后进游戏(或调 resync_custom_tools)即可发现。详见 docs/custom-mcp-tools-guide.md。
#
# 注意:
#   - mcp_tool 由扫描器自动注入命名空间,无需 import。
#   - 这些工具原是 MCDK 早期内置示例,现改为开发者按需自取的范例。
import mod.server.extraServerApi as serverApi
import mod.client.extraClientApi as clientApi


# ── 引擎组件工厂(惰性缓存,避免每次调用重建)──────────────────────
_SR_FACTORY = None


def _SR():
    global _SR_FACTORY
    if _SR_FACTORY is None:
        _SR_FACTORY = serverApi.GetEngineCompFactory()
    return _SR_FACTORY


def _entity_brief(entityId):
    # 取单个实体/玩家的概要信息;逐项容错,单个失败不影响整体。
    factory = _SR()
    info = {"id": entityId}
    try:
        info["type"] = factory.CreateEngineType(entityId).GetEngineTypeStr()
    except Exception:
        info["type"] = None
    try:
        info["dimension"] = factory.CreateDimension(entityId).GetEntityDimensionId()
    except Exception:
        info["dimension"] = None
    try:
        info["name"] = factory.CreateName(entityId).GetName()
    except Exception:
        info["name"] = None
    try:
        info["pos"] = factory.CreatePos(entityId).GetPos()
    except Exception:
        info["pos"] = None
    return info


@mcp_tool(
    name="get_all_entities",
    description="获取所有已加载实体(不含玩家)的列表,每项含 id/type/dimension/name/pos。",
    params=[],
    side="server",
)
def get_all_entities():
    actors = serverApi.GetEngineActor() or {}
    result = []
    for entityId in actors.keys():
        result.append(_entity_brief(entityId))
    return {"count": len(result), "entities": result}


@mcp_tool(
    name="get_all_players",
    description="获取所有玩家列表,每项含 id/type/dimension/name/pos。",
    params=[],
    side="server",
)
def get_all_players():
    players = serverApi.GetPlayerList() or []
    result = []
    for pid in players:
        result.append(_entity_brief(pid))
    return {"count": len(result), "players": result}


@mcp_tool(
    name="get_entity_info",
    description="获取指定实体的详细信息(type/dimension/name/pos)。",
    params=[{"name": "entity_id", "type": "string", "description": "实体ID", "required": True}],
    side="server",
)
def get_entity_info(entity_id=None):
    if not entity_id:
        return {"error": "entity_id is required"}
    return _entity_brief(str(entity_id))


@mcp_tool(
    name="get_player_pos",
    description="获取指定玩家的坐标位置 (x, y, z)。",
    params=[{"name": "player_id", "type": "string", "description": "玩家ID", "required": True}],
    side="server",
)
def get_player_pos(player_id=None):
    if not player_id:
        return {"error": "player_id is required"}
    pos = None
    try:
        pos = _SR().CreatePos(str(player_id)).GetPos()
    except Exception:
        pos = None
    return {"id": player_id, "pos": pos}


@mcp_tool(
    name="get_top_ui",
    description="获取当前 UI 堆栈顶层界面名称。仅 PushScreen 注册界面有名,原生界面返回 None。",
    params=[],
    side="client",
)
def get_top_ui():
    node = clientApi.GetTopUINode()
    name = None
    if node is not None:
        try:
            name = node.GetScreenName()
        except Exception:
            name = None
    return {"top_ui": name, "has_node": node is not None}
