# MCDevTool Premium
适用于**网易我的世界**的开发者工具包，提供创建测试世界、加载用户Mod等功能，方便开发者在脱离**mcs编辑器**的环境下离线测试Mod。

基于MCDevTool开发，原仓库地址：[MCDevTool](https://github.com/GitHub-Zero123/MCDevTool)

本工具保持开源、免费，切勿轻信虚假信息。

![image](./mods/demo2.webp)

## 配置mcdk
您可以将**mcdk**添加到环境变量Path中，也可以直接放置在本地项目工作区以便命令搜索。

> vscode[插件](https://marketplace.visualstudio.com/items?itemName=dofes.mcdev-tools)现已经上线，可直接使用插件一站式开发，无需额外配置。

## 在vscode中使用
您可以在**vscode**中配置任务以便直接运行**mcdk**，例如：

```jsonc
// .vscode/tasks.json
{
    "version": "2.0.0",
    "tasks": [
        {
            // 普通启动模式（根据配置文件，默认自动进入存档）
            "label": "RUN MC DEV",
            "type": "shell",
            "command": "cmd /c mcdk",
            "presentation": {
                "reveal": "always",
                "panel": "shared"
            },
            "problemMatcher": [
                "$python"
            ]
        },
        {
            // 子进程启动模式（必定不会自动进入存档），用于自测联机调试
            "label": "RUN MC SUB DEV",
            "type": "shell",
            "command": "cmd /c mcdk",
            "options": {
                "env": {
                    // 传递环境变量控制mcdk行为
                    "MCDEV_AUTO_JOIN_GAME": "0",
                    "MCDEV_IS_SUBPROCESS_MODE": "1"
                }
            },
            "presentation": {
                "reveal": "always",
                "panel": "shared"
            },
            "problemMatcher": [
                "$python"
            ]
        }
    ]
}
```

## vscode断点调试
您可以通过配置**launch.json**以便在**vscode**中调试Mod代码，例如：

```jsonc
// .vscode/launch.json
// 注：断点支持依赖mcdbg后端，需要在mcdev.json文件中配置启用，另见debugger/README.md
{
    "version": "0.2.0",
    "configurations": [
        {
            // 可通过F5快捷键启动调试器附加
            "name": "Minecraft Modpc Debugger",
            "type": "debugpy",
            "request": "attach",
            "connect": {
                "host": "localhost",
                "port": 5632
            },
            "pathMappings": [
                {
                    "localRoot": "${workspaceFolder}",
                    "remoteRoot": "${workspaceFolder}"
                }
            ],
            "justMyCode": false
        }
    ]
}
```

## 在pycharm中使用
> 注：PyCharm并非该项目主推的IDE，推荐使用`vscode`进行Mod开发与调试。
1. 点击菜单栏中的 `Run → Edit Configurations`
2. 打开`创建 Run Configuration`
3. 创建新的配置项
4. 配置`Shell Script`执行`mcdk`

<!-- ## 在pycharm中调试

> 注：mcdbg后端基于微软的`DAP`协议，**pycharm**仅**专业版**支持`DAP`远程调试，社区版用户请使用**vscode**进行断点调试。

相关参考文档：

- [远程调试配置指南](https://www.jetbrains.com.cn/help/pycharm/remote-debugging-with-product.html)
- [附加到DAP](https://www.jetbrains.com/zh-cn/help/pycharm/run-debug-configuration-attach-to-dap.html)
 -->

## mcdev.json 配置参数
MCDEV配置文件，若不存在字段将以此处默认值为基准。
```jsonc
{
    // 首次运行将会自动生成 .mcdev.json 文件
    // 用于包含需要加载的MOD目录(默认值) 允许相对路径和绝对路径(相对路径以工作区为基准)
    "included_mod_dirs": [
        "./"   // 可以使用 {"path": "./", "hot_reload": true, "enabled": true} 控制包含的目录是否参与热更新检测
    ],
    // 指定游戏exe路径(string)
    "game_executable_path": "",
    // 生成的世界种子 若为null则随机生成(null / int)
    "world_seed": null,
    // 是否在启动时重置并新生成世界
    "reset_world": false,
    // 用于渲染的世界名称 (string)
    "world_name": "MC_DEV_WORLD",
    // 目录存档名(ASCII STRING)
    "world_folder_name": "MC_DEV_WORLD",
    // 是否自动进入游戏存档
    "auto_join_game": true,
    // 是否附加调试MOD(boolean)，若启用将在生成的世界中包含热更新脚本(R键触发检测)并重定向输出流使其附加[Python]前缀可供筛选搜索。
    "include_debug_mod": true,
    // 是否自动热更新MOD
    "auto_hot_reload_mods": true,
    // 生成的世界类型(0.旧版有限世界 1.无限世界 2.超平坦) (int)
    "world_type": 1,
    // 游戏模式(0.生存 1.创造 2.冒险) (int)
    "game_mode": 1,
    // 是否启用作弊(boolean)
    "enable_cheats": true,
    // 是否死亡不掉落(boolean)
    "keep_inventory": true,
    // 天气是否自然更替
    "do_weather_cycle": true,
    // 昼夜是否自然更替
    "do_daylight_cycle": true,
    // 实验性玩法配置
    "experiment_options": {
        // 数据驱动生物群系
        "data_driven_biomes": false,
        // 其他数据型驱动功能
        "data_driven_items": false,
        // 实验性Molang特性
        "experimental_molang_features": false
    },
    // 用户自定义名称(默认"developer")
    "user_name": "developer",
    // 用户自定义皮肤信息（默认缺失字段自动生成）
    "skin_info": {
        "slim": false,
        "skin": "完整贴图路径.png"
    },
    // MODPC调试器配置（依赖mcdbg后端，请确保配置在环境变量/当前工作区）
    "modpc_debugger": {
        // 注：若使用插件一站式解决方案则通常不需要启用此选项，由插件自动管理
        "enabled": false,   // 默认不启用
        "port": 5632        // 端口号（需要在vscode配置中同步）
    },
    // 自定义debug参数(选填可缺失)
    "debug_options": {
        // 键码查阅：https://mc.163.com/dev/mcmanual/mc-dev/mcdocs/1-ModAPI-beta/%E6%9E%9A%E4%B8%BE%E5%80%BC/KeyBoardType.html
        // 绑定热更新快捷键
        "reload_key": "82",
        // 绑定重载世界快捷键
        "reload_world_key": "",
        // 绑定重载Addon快捷键
        "reload_addon_key": "",
        // 绑定重载着色器快捷键
        "reload_shaders_key": "",
        // 是否在全体UI界面都触发热更新快捷键（默认false仅HUD界面）
        "reload_key_global": false
    },
    // 窗口样式（美化类？）
    "window_style": {
        // 悬浮置顶
        "always_on_top": false,
        // 隐藏标题栏
        "hide_title_bar": false,
        // 自定义标题栏颜色 null | [R,G,B]
        "title_bar_color": null,
        // 锁定大小 null | [w, h]
        "fixed_size": null,
        // 锁定屏幕位置 null | [x, y]
        "fixed_position": null,
        // 锁定在屏幕四个脚落（覆盖fixed_position）1. 左上 2. 右上 3. 左下 4. 右下 null | int
        "lock_corner": null
    },
    // 网易独占配置项
    "netease_config": {
        // 是否启用聊天扩展功能（nethard魔改的游戏聊天界面）
        "chat_extension": false
    },
    // MCP服务器配置项
    "mcp_server_config": {
        // 是否启用MCP服务器功能
        // 该MCP提供：日志查询、代码执行、画面捕获、自动化操作、UI调试和自定义工具等功能。
        "enabled": true,
        // 服务器IP地址
        "server_ip": "127.0.0.1",
        // 服务器端口
        "server_port": 19133,
        // 工具开关：可使用工具名逐个关闭，未列出的工具默认启用
        "tools": {
            // 日志查询
            "get_latest_logs": true,
            "get_log_range": true,
            "get_latest_error_logs": true,
            // Python代码执行
            "execute_code": true,
            // 游戏、Addon和着色器重载
            "reload_game": true,
            "reload_addon_and_game": true,
            "reload_all_shaders": true,
            "reload_single_shader": true,
            // 游戏窗口截图和点击
            "capture_game_window": true,
            "click_game_window": true,
            // UI调试
            "ui_control_tree": true,
            "ui_control_get_data": true,
            "ui_control_search": true,
            "ui_locate_control": true,
            "ui_debug_overlay": true,
            "ui_set_visible": true,
            "ui_get_selection": true,
            "ui_wait_for_selection": true,
            "ui_set_debug_enabled": true,
            // 自定义工具组开关
            "custom_tools": true
        }
    }
}
```

## MCP客户端配置

支持标准MCP客户端接入，以下配置以 `Roo Code` 为例。

```jsonc
{
    // Roo Code MCP Settings
    "mcpServers": {
        "minecraft_be_mcdk": {
            "url": "http://localhost:19133/sse",
            "name": "Minecraft(BE) MCP Server(MCDK)"
        }
    }
}
```

### VSCode（Copilot）

VSCode 暂不支持直接连接 SSE，需通过 `mcp-remote` 桥接，配置在 [`.vscode/mcp.json`](.vscode/mcp.json)：

```jsonc
{
    "servers": {
        "minecraft_be_mcdk": {
            // 依赖nodejs环境
            "command": "npx",
            "args": [
                "mcp-remote",
                "http://localhost:19133/sse",
                "--transport",
                "sse-only"
            ]
        }
    }
}
```

> MCP 服务器随 `MCDK/MC` 一起启停，游戏关闭后需重新连接。各客户端对自动重连的支持情况不同，请自行测试。

## UI 调试 MCP

MCDK Premium 内置兼容 Safaia 协议的 UI 调试控制器，可直接读取游戏内控件树和属性，并提供搜索、定位、显隐和交互选取能力。相比只依赖截图和坐标点击，UI 调试工具能够返回稳定的控件路径及结构化属性，更适合排查 JSON UI 层级、布局和可见性问题。

| 工具 | 作用 |
|---|---|
| `ui_control_tree` | 获取当前界面或指定子路径的控件树 |
| `ui_control_get_data` | 批量读取控件位置、尺寸、透明度、层级和组件等属性 |
| `ui_control_search` | 按名称、类型或路径搜索当前控件树 |
| `ui_locate_control` | 在游戏内用红框高亮指定控件 |
| `ui_debug_overlay` | 显示或隐藏全屏控件边界轮廓 |
| `ui_set_visible` | 显示或隐藏指定控件 |
| `ui_get_selection` | 获取最近一次在游戏内选中的控件路径 |
| `ui_wait_for_selection` | 等待用户点击控件并返回最深层控件路径 |
| `ui_set_debug_enabled` | 显式保持或关闭 UI 调试模式 |

UI 调试模式默认关闭，普通点击仍按正常游戏交互执行。读取类工具会在调用期间临时启用调试模式；`ui_locate_control`、`ui_debug_overlay(visible=true)` 等需要保持可视效果的操作会持续启用调试模式。完成后请调用：

```text
ui_set_debug_enabled(enabled=false)
```

也可以对每个 `ui_*` 工具使用 `.mcdev.json` 中同名的 `mcp_server_config.tools` 开关。所有 UI 调试工具均关闭时，Safaia 控制器不会启动。

推荐的排查流程：

1. 使用 `ui_control_tree` 或 `ui_control_search` 找到目标控件路径；
2. 使用 `ui_control_get_data` 检查控件属性；
3. 使用 `ui_locate_control` 配合 `capture_game_window` 做视觉确认；
4. 调试完成后关闭 UI 调试模式，恢复正常操作。

## 自定义 MCP 工具

MCDK Premium 支持使用 `@mcp_tool` 把普通 Python 2.7 函数注册为一等 MCP 工具。工具名称、说明和参数会转换为 MCP schema，客户端可以直接发现并调用，无需每次都通过 `execute_code` 拼接临时代码。

工具文件支持两种作用域：

| 作用域 | 目录 |
|---|---|
| 项目级 | `<included_mod_dir>/mcp_tools/*.py` |
| 全局 | `%USERPROFILE%\.mcdk\mcp_tools\*.py` |

最小示例：

```python
# -*- coding: utf-8 -*-

@mcp_tool(
    name="echo_message",
    description="返回传入的消息",
    params=[
        {"name": "message", "type": "string", "description": "消息内容", "required": True},
    ],
    side="server",
)
def echo_message(message=""):
    return {"echo": message}
```

`mcp_tool` 由扫描器自动注入，不需要也不能单独导入。新增、删除或修改工具文件后，调用 `resync_custom_tools` 即可重新扫描并动态注册/注销工具。stdio bridge 会转发 `tools/list_changed` 通知，支持该通知的客户端可自动刷新工具列表；其他客户端可通过重新连接 MCP 刷新。

系统始终提供 `health_check` 检查游戏侧自定义工具链路。设置 `mcp_server_config.tools.custom_tools=false` 可关闭用户自定义工具和 `resync_custom_tools`，但不会关闭 `health_check`。

> `mcp_tools/` 中的文件会在游戏侧直接执行，定位为受信任的开发者工具，不提供代码沙箱。请勿放入来源不明的脚本。

完整参数、返回值、客户端/服务端线程规则和故障排查见：

- [自定义 MCP 工具开发指南](docs/custom-mcp-tools-guide.md)
- [自定义 MCP 工具示例](docs/examples/mcp_tools_example.py)

## MCP 游戏测试工作流策略

MCDK MCP 的定位不是让通用 Agent 仅凭 LLM、截图和点击完成复杂游戏测试。现阶段更可靠的方式是：在开发代码时预留测试函数、诊断入口和结构化日志，再通过 MCP 客户端 / 服务端代码执行 Tool 触发这些入口，并用日志查询 Tool 收集结果做统计分析。

推荐工作流：

1. 在 Mod / Addon 代码中预留开发期测试函数；
2. 使用 MCP `execute_code` 调用客户端或服务端测试入口；
3. 使用 `get_latest_logs` / `get_latest_error_logs` 收集结构化日志；
4. 多轮执行后统计成功率、耗时和异常分布；
5. 仅在视觉效果本身是测试目标时使用截图和点击能力。

详细规范见 [MCP 游戏测试功能介绍与工作流策略规范](docs/mcp-game-testing-workflow.md)。面向 Agent 的可复用 Skill 位于 [.roo/skills/mcdk-mcp-game-testing-workflow/SKILL.md](.roo/skills/mcdk-mcp-game-testing-workflow/SKILL.md)。

## 第三方依赖
| 库名 | 用途 | 备注 |
|-----|------|------|
| [nlohmann/json](https://github.com/nlohmann/json) | 处理 JSON 配置文件解析与生成 | Header-only |
| [NBT](https://github.com/GlacieTeam/NBT) | 用于构建 `level.dat` 等 NBT 格式文件 | 依赖 BinaryStream 和 Zlib |
| [BinaryStream](https://github.com/GlacieTeam/BinaryStream) | NBT 的底层二进制读写支持 | NBT 内部依赖 |
| [Zlib](https://zlib.net) | NBT 数据压缩与解压缩 | NBT 内部依赖 |
| [CLI11](https://github.com/CLIUtils/CLI11) | 命令行参数解析 | Header-only |
| [cpp-mcp](https://github.com/hkr04/cpp-mcp) | 实现 MCP 协议的服务器功能 | 魔改扩展协议 |
