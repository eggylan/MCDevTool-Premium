#pragma once

#include <mcp_tool.h>

namespace mcdk::mcp_tool_definitions {

    inline constexpr const char* GetLatestLogsName = "get_latest_logs";
    inline constexpr const char* GetLatestLogsDescription = R"(Returns the most recent game log entries.

Parameters:
- max_count: Maximum number of log entries to return
- order: "asc" for oldest to newest
         "desc" for newest to oldest
)";

    inline constexpr const char* GetLogRangeName = "get_log_range";
    inline constexpr const char* GetLogRangeDescription = R"TAG(Returns a specific range of recent game log entries by index.

Logs are indexed relative to the most recent entry:
- index 0 refers to the newest log
- index 1 refers to the second newest log
- and so on

Parameters:
- start_index: Starting index (inclusive)
- end_index: Ending index (exclusive)
- order: "asc" for oldest to newest
         "desc" for newest to oldest)TAG";

    inline constexpr const char* GetLatestErrorLogsName = "get_latest_error_logs";
    inline constexpr const char* GetLatestErrorLogsDescription = R"(Returns stderr error log entries only. May not include all errors (e.g., JSON parsing errors). Use get_latest_logs for complete logs.

Parameters:
- max_count: Maximum number of log entries to return
- order: "asc" for oldest to newest, "desc" for newest to oldest
)";

    inline constexpr const char* ExecuteCodeName = "execute_code";
    inline constexpr const char* ExecuteCodeDescription = R"(Executes provided code in the game environment.
Parameters:
- code: The code to Py2 execute. Expression code returns the expression value; statement code may assign _result to define the returned value.
- is_client: Whether to execute on client side (true) or server side (false)
- direct_return: Whether to wait for and directly return the execution result (default true). Set false to use the legacy async log-based behavior.)";

    inline constexpr const char* ReloadGameName = "reload_game";
    inline constexpr const char* ReloadGameDescription =
        "Reloads the game environment. Use with caution and only when necessary. Python code supports "
        "incremental hot-reload, so a full reload should be avoided unless hot-reload is insufficient.";

    inline constexpr const char* ReloadAddonAndGameName = "reload_addon_and_game";
    inline constexpr const char* ReloadAddonAndGameDescription =
        "Reloads both the game environment and the addon data. Use this when you have made changes to "
        "addon resources (e.g., textures, sounds) that require a full reload to take effect.";

    inline constexpr const char* ReloadAllShadersName = "reload_all_shaders";
    inline constexpr const char* ReloadAllShadersDescription =
        "Triggers a reload of all shaders. This is a heavy operation and may cause significant lag. "
        "Use only when necessary, such as after modifying shader files. There is no direct feedback "
        "when the reload is complete, so please verify the result visually in the game.";

    inline constexpr const char* ReloadSingleShaderName = "reload_single_shader";
    inline constexpr const char* ReloadSingleShaderDescription = R"(Triggers a reload of a single shader by filename. This is faster than reloading all shaders and can be used for quicker iteration when only one shader file is modified.
Parameters:
- file_name: The name of the shader file to reload, relative to the shaders directory.
For example, "entity.fragment" or "block.vertex". Do not include the file extension.)";

    inline constexpr const char* CaptureGameWindowName = "capture_game_window";
    inline constexpr const char* CaptureGameWindowDescription =
        "Captures the current Minecraft game window as a 480p JPEG screenshot and returns base64-encoded image data. "
        "This is a relatively expensive visual inspection tool and can distract from code/log based debugging. "
        "Prefer get_latest_logs, get_latest_error_logs, and deterministic file/code checks first; use screenshots only "
        "when the task explicitly requires visual confirmation or logs cannot answer the question.";

    inline constexpr const char* ClickGameWindowName = "click_game_window";    inline constexpr const char* ClickGameWindowDescription = R"(Simulates a left mouse click at a specific position on the Minecraft game window.

Coordinates are percentage-based (0.0 to 1.0) relative to the client area:
- (0.0, 0.0) = top-left corner
- (0.5, 0.5) = center
- (1.0, 1.0) = bottom-right corner

The coordinate system matches the capture_game_window screenshot exactly.
The game window will be brought to the foreground automatically before clicking.

Parameters:
- x: Horizontal position as a percentage (0.0-1.0)
- y: Vertical position as a percentage (0.0-1.0))";

    // ── UI 调试工具（Safaia 控制器）────────────────────────────────
    inline constexpr const char* UiControlTreeName = "ui_control_tree";
    inline constexpr const char* UiControlTreeDescription = R"(Returns the in-game UI control tree (name/type/visible/children) for the current screen via the embedded Safaia UI debugger.

Parameters:
- root_path: Subtree root, e.g. "/" (default) or "/ScreenName/panel". Illegal roots time out.
- force_refresh: If true, bypass the cache and re-fetch from the game (default false). The cache is invalidated automatically when the screen changes.)";

    inline constexpr const char* UiControlGetDataName = "ui_control_get_data";
    inline constexpr const char* UiControlGetDataDescription = R"(Returns the full property bag for the given control paths (position/size/alpha/layer/components/etc.) via the Safaia UI debugger.

Parameters:
- paths: Array of control paths (e.g. ["/ScreenName/panel/label"]). Keep each batch <= ~200 paths.)";

    inline constexpr const char* UiControlSearchName = "ui_control_search";
    inline constexpr const char* UiControlSearchDescription = R"(Searches the current UI control tree locally (no remote search RPC) and returns matching controls.

Parameters:
- keyword: Substring to match (case-insensitive).
- by: Field to match against: "name" (default), "type", or "path".
- limit: Max results to return (default 50).)";

    inline constexpr const char* UiLocateControlName = "ui_locate_control";
    inline constexpr const char* UiLocateControlDescription = R"(Highlights the given controls in-game with a red selection box (Safaia SetSelectedControls). Fire-and-forget; confirm visually with capture_game_window or via ui_get_selection.

Parameters:
- paths: Array of control paths to highlight.)";

    inline constexpr const char* UiDebugOverlayName = "ui_debug_overlay";
    inline constexpr const char* UiDebugOverlayDescription = R"(Toggles the full-screen control-bounds outline overlay in-game (Safaia SetBoundsVisible). Fire-and-forget.

Parameters:
- visible: true to show all control outlines, false to hide.)";

    inline constexpr const char* UiSetVisibleName = "ui_set_visible";
    inline constexpr const char* UiSetVisibleDescription = R"(Shows or hides a single UI control (Safaia SetControlVisible). Fire-and-forget; re-check with ui_control_get_data.

Parameters:
- path: Control path to toggle.
- visible: true to show, false to hide.)";

    inline constexpr const char* UiGetSelectionName = "ui_get_selection";
    inline constexpr const char* UiGetSelectionDescription =
        "Returns the most recently selected control path(s) captured from the game's in-debugger clicks "
        "(Safaia ControlSelectionChanged). Returns immediately with the last known selection.";

    inline constexpr const char* UiWaitForSelectionName = "ui_wait_for_selection";
    inline constexpr const char* UiWaitForSelectionDescription = R"(Blocks until the next in-game control selection (a debugger click selects the deepest leaf), or until timeout. Use this to ask the user to click a control and capture its path.

Parameters:
- timeout: Seconds to wait (default 30, max 120).)";

    inline mcp::tool buildGetLatestLogsTool() {
        return mcp::tool_builder(GetLatestLogsName)
            .with_description(GetLatestLogsDescription)
            .with_number_param("max_count", "Maximum number of log entries to return", false)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildGetLogRangeTool() {
        return mcp::tool_builder(GetLogRangeName)
            .with_description(GetLogRangeDescription)
            .with_number_param("start_index", "Starting index (inclusive)", true)
            .with_number_param("end_index", "Ending index (exclusive)", true)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildGetLatestErrorLogsTool() {
        return mcp::tool_builder(GetLatestErrorLogsName)
            .with_description(GetLatestErrorLogsDescription)
            .with_number_param("max_count", "Maximum number of log entries to return", false)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildExecuteCodeTool() {
        return mcp::tool_builder(ExecuteCodeName)
            .with_description(ExecuteCodeDescription)
            .with_string_param("code", "Code to execute", true)
            .with_boolean_param("is_client", "Execute on client side?", false)
            .with_boolean_param("direct_return", "Directly return execution result instead of relying on logs? Default true.", false)
            .build();
    }

    inline mcp::tool buildReloadGameTool() {
        return mcp::tool_builder(ReloadGameName)
            .with_description(ReloadGameDescription)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildReloadAddonAndGameTool() {
        return mcp::tool_builder(ReloadAddonAndGameName)
            .with_description(ReloadAddonAndGameDescription)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildReloadAllShadersTool() {
        return mcp::tool_builder(ReloadAllShadersName)
            .with_description(ReloadAllShadersDescription)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildReloadSingleShaderTool() {
        return mcp::tool_builder(ReloadSingleShaderName)
            .with_description(ReloadSingleShaderDescription)
            .with_string_param("file_name", "Name of the shader file to reload (e.g., 'entity.fragment')", true)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildCaptureGameWindowTool() {
        return mcp::tool_builder(CaptureGameWindowName)
            .with_description(CaptureGameWindowDescription)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildClickGameWindowTool() {
        return mcp::tool_builder(ClickGameWindowName)
            .with_description(ClickGameWindowDescription)
            .with_number_param("x", "Horizontal position (0.0=left, 1.0=right)", true)
            .with_number_param("y", "Vertical position (0.0=top, 1.0=bottom)", true)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildUiControlTreeTool() {
        return mcp::tool_builder(UiControlTreeName)
            .with_description(UiControlTreeDescription)
            .with_string_param("root_path", "Subtree root path (default \"/\")", false)
            .with_boolean_param("force_refresh", "Bypass cache and re-fetch (default false)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildUiControlGetDataTool() {
        return mcp::tool_builder(UiControlGetDataName)
            .with_description(UiControlGetDataDescription)
            .with_array_param("paths", "Control paths to fetch properties for", "string", true)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildUiControlSearchTool() {
        return mcp::tool_builder(UiControlSearchName)
            .with_description(UiControlSearchDescription)
            .with_string_param("keyword", "Substring to match (case-insensitive)", true)
            .with_string_param("by", "Match field: name | type | path (default name)", false)
            .with_number_param("limit", "Max results (default 50)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildUiLocateControlTool() {
        return mcp::tool_builder(UiLocateControlName)
            .with_description(UiLocateControlDescription)
            .with_array_param("paths", "Control paths to highlight", "string", true)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildUiDebugOverlayTool() {
        return mcp::tool_builder(UiDebugOverlayName)
            .with_description(UiDebugOverlayDescription)
            .with_boolean_param("visible", "Show (true) or hide (false) all control outlines", true)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildUiSetVisibleTool() {
        return mcp::tool_builder(UiSetVisibleName)
            .with_description(UiSetVisibleDescription)
            .with_string_param("path", "Control path to toggle", true)
            .with_boolean_param("visible", "Show (true) or hide (false)", true)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildUiGetSelectionTool() {
        return mcp::tool_builder(UiGetSelectionName)
            .with_description(UiGetSelectionDescription)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildUiWaitForSelectionTool() {
        return mcp::tool_builder(UiWaitForSelectionName)
            .with_description(UiWaitForSelectionDescription)
            .with_number_param("timeout", "Seconds to wait (default 30, max 120)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline std::vector<mcp::tool> buildAllTools() {
        return {
            buildGetLatestLogsTool(),
            buildGetLogRangeTool(),
            buildGetLatestErrorLogsTool(),
            buildExecuteCodeTool(),
            buildReloadGameTool(),
            buildReloadAddonAndGameTool(),
            buildReloadAllShadersTool(),
            buildReloadSingleShaderTool(),
            buildCaptureGameWindowTool(),
            buildClickGameWindowTool(),
            buildUiControlTreeTool(),
            buildUiControlGetDataTool(),
            buildUiControlSearchTool(),
            buildUiLocateControlTool(),
            buildUiDebugOverlayTool(),
            buildUiSetVisibleTool(),
            buildUiGetSelectionTool(),
            buildUiWaitForSelectionTool(),
        };
    }

} // namespace mcdk::mcp_tool_definitions
