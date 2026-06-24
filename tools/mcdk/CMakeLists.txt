project(MC_DEV_TOOL_MCDK)

option(MC_DEV_TOOL_MCDK_ENABLE_CLI "BUILD CLI EXECUTABLE (CLI11 subcommand mode)" OFF)

# ══════════════════════════════════════════════════════════════════════════
# mcdk.exe（no-GUI，与 main 分支行为对齐）
# ─ 永不链 Qt、不探测 Qt；走 CoreServices + GameLauncher 的 runNoGui 路径。
# ─ 共享 mcdevtool OBJECT 库 + mcp + MCDEV_MOD_RESOURCE。
# ══════════════════════════════════════════════════════════════════════════
add_executable(mcdk main_cli.cpp)
target_compile_features(mcdk PRIVATE cxx_std_23)
target_link_libraries(mcdk PRIVATE mcdevtool)
target_link_libraries(mcdk PRIVATE mcp)
target_link_libraries(mcdk PRIVATE MCDEV_MOD_RESOURCE)

if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
    # 决策 8：CLI 子模式两个 exe 都可启用（默认 OFF）。
    target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_CLI)
    target_sources(mcdk PRIVATE cli.cpp)
endif()

# ══════════════════════════════════════════════════════════════════════════
# mcdk-gui.exe（带 Qt GUI，与当前 feature/qt-gui 状态一致）
# ─ 仅在 MC_DEV_TOOL_ENABLE_GUI=ON 时构建（默认 ON）。
# ─ find_package(Qt6) 严格限定在此分支内，no-GUI 路径零 Qt 探测。
# ─ windeployqt 把 Qt 运行时部署到 exe 同目录，与 mcdk.exe 共享。
# ══════════════════════════════════════════════════════════════════════════
if(MC_DEV_TOOL_ENABLE_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)

    add_executable(mcdk_gui main_gui.cpp
        modules/gui/app.cpp
        modules/gui/code_exec_panel.cpp
        modules/gui/log_bridge.cpp
        modules/gui/log_console_panel.cpp
        modules/gui/main_window.cpp
        modules/gui/python_highlighter.cpp
        modules/gui/theme_manager.cpp
    )
    target_compile_features(mcdk_gui PRIVATE cxx_std_23)
    target_link_libraries(mcdk_gui PRIVATE mcdevtool)
    target_link_libraries(mcdk_gui PRIVATE mcp)
    target_link_libraries(mcdk_gui PRIVATE MCDEV_MOD_RESOURCE)
    target_link_libraries(mcdk_gui PRIVATE Qt6::Widgets)

    set_target_properties(mcdk_gui PROPERTIES
        AUTOMOC ON
        AUTORCC ON
    )

    # MCDK_ENABLE_GUI 宏守护所有 modules/gui/* 中的 GUI 代码。
    # QT_NO_KEYWORDS 避免与 Python/C++ slots 关键字冲突（沿用原约定）。
    target_compile_definitions(mcdk_gui PRIVATE MCDK_ENABLE_GUI QT_NO_KEYWORDS)

    if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
        target_compile_definitions(mcdk_gui PRIVATE MCDK_ENABLE_CLI)
        target_sources(mcdk_gui PRIVATE cli.cpp)
    endif()

    if(WIN32)
        # windeployqt 必须用 Qt 安装目录下的全路径，禁止走 PATH（Anaconda Qt5 误伤）。
        get_filename_component(_qt_cmake_dir "${Qt6_DIR}" DIRECTORY)
        get_filename_component(_qt_lib_dir "${_qt_cmake_dir}" DIRECTORY)
        get_filename_component(_qt_prefix_dir "${_qt_lib_dir}" DIRECTORY)
        set(_qt_windeployqt "${_qt_prefix_dir}/bin/windeployqt.exe")
        if(EXISTS "${_qt_windeployqt}")
            add_custom_command(TARGET mcdk_gui POST_BUILD
                COMMAND "${_qt_windeployqt}"
                    --no-translations
                    $<$<CONFIG:Debug>:--debug>
                    $<$<NOT:$<CONFIG:Debug>>:--release>
                    "$<TARGET_FILE:mcdk_gui>"
                COMMENT "Deploying Qt runtime for mcdk-gui"
            )
        else()
            message(WARNING "windeployqt not found at ${_qt_windeployqt}; Qt runtime deployment skipped")
        endif()
    endif()
endif()

# ─── Clang 警告抑制（两个 exe 共用） ───
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    foreach(_tgt mcdk mcdk_gui)
        if(TARGET ${_tgt})
            target_compile_options(${_tgt} PRIVATE -Wno-implicit-const-int-float-conversion)
        endif()
    endforeach()
endif()
