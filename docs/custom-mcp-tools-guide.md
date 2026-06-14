# 自定义 MCP 工具开发指南

> 适用于 MCDK 的自定义 MCP 工具系统：用 `@mcp_tool` 装饰普通 Python 函数，mcdk 会把它注册成
> **一等 MCP 工具**,Claude 等客户端可直接调用,支持热更新与项目级 / 全局两种作用域。

---

## 1. 这是什么

MCDK 在游戏侧调试 MOD(`DEBUG_ENV_SCRIPT`)内提供 `@mcp_tool` 装饰器。你写一个普通的
Py2.7 函数并装饰它,mcdk 启动游戏后会:

1. **发现**:扫描约定目录,把装饰过的函数收集进注册表;
2. **上报**:经 IPC 把注册表交给 mcdk;
3. **注册**:mcdk 把每个工具注册成独立的 MCP 工具(名称 / 描述 / 参数 schema 来自装饰器);
4. **调用**:Claude 调用该工具 → mcdk 经 IPC 转发到游戏侧对应函数 → 返回值统一转成字符串回传。

与"在 `execute_code` 里写一大段代码"相比,自定义工具的优势是:**接口固定、参数有 schema、
可复用、AI 可直接按名调用**,适合把高频调试 / 测试操作固化下来。

---

## 2. 快速开始

### 2.1 放在哪里

工具文件放在 `mcp_tools/` 目录下的 `.py` 文件里(文件名不要以 `__` 开头)。支持两种作用域:

| 作用域 | 位置 | 适用 |
|---|---|---|
| **项目级** | `<你的Mod>/mcp_tools/*.py`(`<你的Mod>` = `.mcdev.json` 的 `included_mod_dirs` 解析出的目录) | 仅当前项目用的工具 |
| **全局** | `%USERPROFILE%\.mcdk\mcp_tools\*.py`(mcdk 启动时自动创建) | 所有项目共享、定义一次处处可用 |

两个作用域会**同时扫描**(去重)。全局目录可在 `.mcdev.json` 用 `global_mcp_tools_dir`
覆盖路径,或设为空串 `""` 禁用。

```
<你的Mod>/
├── .mcdev.json
└── mcp_tools/
    └── my_tools.py          ← 项目级工具

%USERPROFILE%/.mcdk/mcp_tools/
└── shared_tools.py          ← 全局工具(所有项目可见)
```

### 2.2 写一个工具

```python
# -*- coding: utf-8 -*-
import mod.server.extraServerApi as serverApi


@mcp_tool(
    name="get_player_position",          # MCP 工具名(AI 看到的名字,需唯一)
    description="获取指定玩家的当前坐标",     # 给 AI 的说明
    params=[                              # 参数声明 → 成为 inputSchema,按名传给函数
        {"name": "player_id", "type": "string", "description": "玩家ID", "required": True},
    ],
    side="server",                        # "server" 用 serverApi / "client" 用 clientApi
)
def get_player_position(player_id=None):
    comp = serverApi.GetEngineCompFactory().CreatePos(str(player_id))
    pos = comp.GetPos()                   # (x, y, z)
    return {"player_id": player_id, "pos": pos}   # 返回原生对象即可
```

> **重要:`mcp_tool` 由扫描器自动注入到文件命名空间,不要 `import` 它。** 这些文件是被
> `compile + exec`(注入装饰器)执行的,而不是当普通模块导入,因此相对 import 不可用。

### 2.3 让它生效

| 操作 | 怎么生效 |
|---|---|
| **首次 / 启动前就放好** | 进游戏后连接时自动发现并注册 |
| **运行时新增 / 删除工具文件** | 调用 MCP 工具 `resync_custom_tools` → 动态注册 / 注销,然后客户端 `/mcp` 重连刷新列表 |
| **改已有工具的函数体** | 同样调 `resync_custom_tools` → 重新扫描即生效,**无需重连、无需重启游戏** |

---

## 3. `@mcp_tool` 参数详解

```python
mcp_tool(name, description="", params=None, side="server")
```

| 字段 | 说明 |
|---|---|
| `name` | **必填**,MCP 工具名,全局唯一。重名会互相覆盖。 |
| `description` | 给 AI 的功能说明,写清楚用途和参数含义,AI 据此决定何时调用。 |
| `params` | 参数声明列表(见下),决定 inputSchema 和传给函数的关键字参数。可空 `[]`。 |
| `side` | `"server"`(默认)在服务端线程执行(用 `serverApi`);`"client"` 在客户端线程执行(用 `clientApi`)。框架自动用 `CALL_ON_SERVER_THREAD` / `CALL_ON_CLIENT_THREAD` 切到正确线程,**函数体里无需自己处理线程**。 |

### params 单项格式

```python
{"name": "count", "type": "number", "description": "数量", "required": False}
```

| 字段 | 说明 |
|---|---|
| `name` | 参数名,会作为**关键字参数**传给函数(建议函数签名给默认值)。 |
| `type` | `string` / `number` / `boolean` / `array` / `object` 之一。 |
| `description` | 参数说明。 |
| `required` | 是否必填(默认 `False`)。 |

> 函数签名应与 params 对应,例如 `params=[{"name":"player_id",...}]` 对应
> `def foo(player_id=None):`。给默认值可避免 AI 漏传参数时报错。

---

## 4. 返回值契约

函数返回**原生对象即可**,框架在 MOD 侧统一转成字符串(`return_string`)再交给 AI,**永不抛异常**:

| 返回类型 | 转换方式 |
|---|---|
| `str` / `unicode` | 原样透传(保证 UTF-8) |
| `dict` / `list` / `tuple` | `json.dumps(..., ensure_ascii=False, indent=2)` → 结构化 JSON 文本 |
| `int` / `long` / `float` / `bool` / `None` | `str()`(`None` → `"None"`) |
| 其它 / 自定义对象 | 转 `{"__type__","__repr__"}` 再 `json.dumps`;最终兜底 `repr()` |

推荐返回 `dict` / `list`,AI 拿到的是清晰的结构化 JSON 文本。深度超过 8 层会被折叠为
`{"__type__","__repr__"}`(防止超大 / 循环引用)。

---

## 5. 内置工具

调试 MOD 只内置一个工具,用于验证自定义工具链路是否正常:

| 工具 | side | 说明 |
|---|---|---|
| `health_check` | server | 健康检查:返回 `ok`、服务端/客户端就绪状态、已注册自定义工具数与列表、时间戳。 |

其余工具一律由开发者自行定义。仓库提供一份现成示例(获取实体 / 玩家 / 坐标 / 顶层 UI 等),
直接复制到你的 `mcp_tools/` 目录即可用:

- **`docs/examples/mcp_tools_example.py`** —— 含 `get_all_entities` / `get_all_players` /
  `get_entity_info` / `get_player_pos` / `get_top_ui`,可整文件复制或按需取用。

---

## 6. 完整示例

### 6.1 无参数工具

```python
# -*- coding: utf-8 -*-
import mod.server.extraServerApi as serverApi


@mcp_tool(name="count_loaded_entities", description="统计当前已加载实体数量", params=[], side="server")
def count_loaded_entities():
    actors = serverApi.GetEngineActor() or {}
    return {"count": len(actors)}
```

### 6.2 带参数 + 客户端工具

```python
# -*- coding: utf-8 -*-
import mod.client.extraClientApi as clientApi


@mcp_tool(
    name="notify_player",
    description="在客户端左上角弹出一条提示消息",
    params=[{"name": "msg", "type": "string", "description": "消息内容", "required": True}],
    side="client",
)
def notify_player(msg=""):
    import gui
    gui.set_left_corner_notify_msg(msg)
    return "ok: " + msg
```

### 6.3 多参数 + 缓存工厂(推荐写法)

```python
# -*- coding: utf-8 -*-
import mod.server.extraServerApi as serverApi

_FACTORY = None  # 工厂惰性缓存,避免每次调用重建(性能规范)


def _factory():
    global _FACTORY
    if _FACTORY is None:
        _FACTORY = serverApi.GetEngineCompFactory()
    return _FACTORY


@mcp_tool(
    name="teleport_player",
    description="把玩家传送到指定坐标",
    params=[
        {"name": "player_id", "type": "string", "description": "玩家ID", "required": True},
        {"name": "x", "type": "number", "description": "X", "required": True},
        {"name": "y", "type": "number", "description": "Y", "required": True},
        {"name": "z", "type": "number", "description": "Z", "required": True},
    ],
    side="server",
)
def teleport_player(player_id=None, x=0, y=0, z=0):
    ok = _factory().CreatePos(str(player_id)).SetPos((float(x), float(y), float(z)))
    return {"player_id": player_id, "pos": [x, y, z], "ok": bool(ok)}
```

---

## 7. 相关 MCP 工具

| 工具 | 作用 |
|---|---|
| `resync_custom_tools` | 重扫所有工具目录(项目 + 全局),动态注册 / 注销变化的工具并广播 `tools/list_changed`。新增 / 删除 / 改函数体后调用它。 |

> 调用 `resync_custom_tools` 后,若客户端没有自动刷新工具列表,执行 `/mcp` 重连即可看到增删。

---

## 8. 内置工具开关(`mcp_server_config.tools`)

内置工具在 `.mcdev.json` 配置是否启用,**逐个工具单独开关**(`custom_tools` 例外,是整组开关)。
**当前工作目录没有 `.mcdev.json` 时,mcdk 会自动创建并默认启用 MCP 与全部工具**;已有配置中未列出
的工具也默认启用(向后兼容)。

```jsonc
{
  "mcp_server_config": {
    "enabled": true,
    "server_ip": "127.0.0.1",
    "server_port": 19133,
    "tools": {
      // —— 日志 ——
      "get_latest_logs": true,
      "get_log_range": true,
      "get_latest_error_logs": true,
      // —— 代码执行 ——
      "execute_code": true,
      // —— 重载 ——
      "reload_game": true,
      "reload_addon_and_game": true,
      "reload_all_shaders": true,
      "reload_single_shader": true,
      // —— 游戏窗口 ——
      "capture_game_window": true,
      "click_game_window": true,
      // —— UI 调试(任一启用即启动 Safaia 控制器)——
      "ui_control_tree": true,
      "ui_control_get_data": true,
      "ui_control_search": true,
      "ui_locate_control": true,
      "ui_debug_overlay": true,
      "ui_set_visible": true,
      "ui_get_selection": true,
      "ui_wait_for_selection": true,
      "ui_set_debug_enabled": true,
      // —— 自定义工具组(整组开关:@mcp_tool 发现 + resync_custom_tools)——
      "custom_tools": true
    }
  }
}
```

说明:
- 把某工具设为 `false` 即不注册该工具;未列出的工具默认启用。
- 所有 `ui_*` 全部为 `false` 时,Safaia UI 调试控制器不会启动。
- `custom_tools=false` 时,不发现/注册任何用户 `@mcp_tool` 工具,也不提供 `resync_custom_tools`。
- **`health_check` 始终启用、不可关闭**,不出现在 `tools` 配置中(用于随时确认链路可用)。

---

## 9. 约束与注意事项

1. **Python 2.7 兼容**:禁止 f-string、类型注解(type hints)、`async/await`;`print` 当语句用。
2. **客户端 / 服务端分离**:`side="server"` 的函数体只用 `serverApi`,`side="client"` 只用
   `clientApi`;不要在一个函数里混用两端 API。
3. **不要 `import mcp_tool`**:装饰器由扫描器注入命名空间(见 §2.2)。
4. **`GetEngineCompFactory` 应缓存**:不要在每次调用里重复创建工厂(见 §6.3)。
5. **任意代码执行风险**:`mcp_tools/` 里的代码会被游戏侧直接执行,**定位为开发者工具、不做沙箱**;
   只放你信任的代码,全局目录尤其注意。
6. **超时**:工具默认 10s 执行超时(在游戏线程上跑),长任务请自行拆分或异步化。
7. **发现失败不影响其它工具**:单个文件 / 函数报错会被记录到扫描错误列表(可在
   `list_custom_tools` 返回的 `errors` 字段看到),不会中断其它工具的注册。

---

## 10. 故障排查

| 现象 | 排查 |
|---|---|
| 工具没出现在 `tools/list` | 调 `resync_custom_tools` 后 `/mcp` 重连;确认文件在 `mcp_tools/` 下且不以 `__` 开头;确认 `@mcp_tool` 的 `name` 没和别的工具重名。 |
| 调用报 "game not connected" | 游戏没进世界 / 已退出,IPC 未连接。进游戏后重试。 |
| 全局工具不生效 | 确认文件在 `%USERPROFILE%\.mcdk\mcp_tools\`;启动前就位才会在连接时自动发现,运行时新增需 `resync_custom_tools`;检查 `.mcdev.json` 未把 `global_mcp_tools_dir` 设为空串。 |
| 改了函数体没变化 | 调 `resync_custom_tools`(它会重新 exec 文件);内置工具(`McpToolsBuiltin.py`)属于嵌入脚本,改它需重新编译 mcdk + 重启游戏,而 `mcp_tools/` 下的用户工具则可热更新。 |
| 返回乱码 | 文件头加 `# -*- coding: utf-8 -*-`;中文返回值用 `unicode` 或确保 UTF-8。 |
