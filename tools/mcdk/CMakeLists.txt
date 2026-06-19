project(MC_DEV_TOOL_MCDK)

option(MC_DEV_TOOL_MCDK_ENABLE_CLI "BUILD CLI EXECUTABLE" OFF)

# add_subdirectory(libs/cpp-mcp)

add_executable(mcdk main.cpp)
target_compile_features(mcdk PRIVATE cxx_std_23)
target_link_libraries(mcdk PRIVATE mcdevtool)
target_link_libraries(mcdk PRIVATE mcp)
target_link_libraries(mcdk PRIVATE MCDEV_MOD_RESOURCE)
# target_include_directories(mcdk PRIVATE
#     # ${CMAKE_CURRENT_SOURCE_DIR}/mods/Resource
#     ${CMAKE_CURRENT_SOURCE_DIR}/libs/cpp-mcp/include
#     ${CMAKE_CURRENT_SOURCE_DIR}/libs/cpp-mcp/common
# )

if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
    target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_CLI)
    target_sources(mcdk PRIVATE cli.cpp)
endif()

if(MC_DEV_TOOL_ENABLE_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)

    set_target_properties(mcdk PROPERTIES
        AUTOMOC ON
        AUTORCC ON
    )

    target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_GUI QT_NO_KEYWORDS)
    target_sources(mcdk PRIVATE
        modules/gui/app.cpp
        modules/gui/main_window.cpp
        modules/gui/theme_manager.cpp
    )
    target_link_libraries(mcdk PRIVATE Qt6::Widgets)

    if(WIN32)
        get_filename_component(_qt_cmake_dir "${Qt6_DIR}" DIRECTORY)
        get_filename_component(_qt_lib_dir "${_qt_cmake_dir}" DIRECTORY)
        get_filename_component(_qt_prefix_dir "${_qt_lib_dir}" DIRECTORY)
        set(_qt_windeployqt "${_qt_prefix_dir}/bin/windeployqt.exe")
        if(EXISTS "${_qt_windeployqt}")
            add_custom_command(TARGET mcdk POST_BUILD
                COMMAND "${_qt_windeployqt}"
                    --no-translations
                    $<$<CONFIG:Debug>:--debug>
                    $<$<NOT:$<CONFIG:Debug>>:--release>
                    "$<TARGET_FILE:mcdk>"
                COMMENT "Deploying Qt runtime for mcdk"
            )
        else()
            message(WARNING "windeployqt not found at ${_qt_windeployqt}; Qt runtime deployment skipped")
        endif()
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mcdk PRIVATE
        -Wno-implicit-const-int-float-conversion
    )
endif()
