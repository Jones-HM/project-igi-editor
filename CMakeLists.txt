cmake_minimum_required(VERSION 3.16)
# target_precompile_headers
#   Added in version 3.16

project (igi_editor
  VERSION 3.0
  LANGUAGES C CXX)

if (NOT WIN32)
  message(FATAL_ERROR
    "igi-editor gameplay mode is supported on Windows only; configure with a Windows toolchain.")
endif()

# Designated initializers, introduced in C++20
set(CMAKE_CXX_STANDARD 20)

# enable FOLDER property
#   seperate projects into different groups
SET_PROPERTY(GLOBAL PROPERTY USE_FOLDERS ON)

if (CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release")
endif ()

message("CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_definitions(-D_DEBUG)
    message("Set cmake debug")
endif ()

if (MSVC)
  add_compile_options("/FS")
endif()


# set output binary file directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

if (WIN32)
  # set using UNICODE characterset
  add_definitions(-DUNICODE -D_UNICODE)
endif()

file(GLOB SOURCES "source/*.h" "source/*.cpp")
file(GLOB SOURCES_LEVEL "source/level/*.*")
file(GLOB SOURCES_RUNTIME "source/runtime/*.*")
file(GLOB SOURCES_RENDERER "source/renderer/*.*")
file(GLOB SOURCES_CLI "source/cli/*.*")
file(GLOB SHADERS_41 "shaders/41/*.*")
file(GLOB SHADERS_45 "shaders/45/*.*")

# Add Windows icon resource file
if (WIN32)
  list(APPEND SOURCES "assets/igi-editor.rc")
endif()

source_group("level" FILES ${SOURCES_LEVEL})
source_group("runtime" FILES ${SOURCES_RUNTIME})
source_group("renderer" FILES ${SOURCES_RENDERER})
source_group("cli" FILES ${SOURCES_CLI})
source_group("shaders/41" FILES ${SHADERS_41})
source_group("shaders/45" FILES ${SHADERS_45})

add_executable(igi-editor ${SOURCES} ${SOURCES_LEVEL} ${SOURCES_RUNTIME} ${SOURCES_RENDERER} ${SOURCES_CLI} ${SHADERS_41} ${SHADERS_45})

if (WIN32)
  set_target_properties(igi-editor PROPERTIES
    ICON ${PROJECT_SOURCE_DIR}/assets/igi-editor.ico
    OUTPUT_NAME "igi1ed"
  )
  if (MSVC)
    target_link_options(igi-editor PRIVATE "/LARGEADDRESSAWARE")
  endif()
endif()

target_precompile_headers(igi-editor PRIVATE source/pch.h)

# A few translation units pull in very large header graphs (glm, tinygltf/json,
# renderer stacks). MSVC can exhaust virtual memory while trying to map the
# shared PCH for all of them at once, especially on Release builds.
# Let these heavy files compile without the shared PCH to keep memory usage
# stable while preserving PCH speed for the rest of the project.
set_source_files_properties(
  source/app.cpp
  source/app_input.cpp
  source/app_input_mouse.cpp
  source/app_input_keyboard.cpp
  source/app_view.cpp
  source/app_lookup.cpp
  source/app_editor.cpp
  source/app_ui.cpp
  source/app_level.cpp
  source/renderer/renderer_flat_sky_layers.cpp
  source/renderer/renderer_objects.cpp
  source/renderer/renderer_objects_atta.cpp
  source/renderer/renderer_objects_mesh.cpp
  source/renderer/renderer_objects_metadata.cpp
  source/renderer/renderer_objects_picking.cpp
  source/renderer/renderer_objects_texture.cpp
  source/renderer/renderer_objects_visual.cpp
  source/renderer/renderer_rain.cpp
  source/renderer/renderer_skydome.cpp
  source/renderer/renderer_splines.cpp
  source/renderer/renderer_terrain.cpp
  source/renderer/wic_loader.cpp
  source/level/qvm_parser.cpp
  source/renderer/res_writer.cpp
  source/level/res_model_set.cpp
  source/renderer/res_compiler.cpp
  source/renderer/dat_writer.cpp
  source/level/mtp_writer.cpp
  source/renderer/tex_writer.cpp
  source/renderer/graph_writer.cpp
  source/cli/asset_extractor.cpp
  source/level/terrain.cpp
  source/level/terrain_io.cpp
  source/level/terrain_lod.cpp
  source/level/terrain_mesh.cpp
  source/level/terrain_query.cpp
  source/level/qvm_decompiler.cpp
  source/level/qsc_lexer.cpp
  source/level/qsc_parser.cpp
  source/level/qvm_compiler.cpp
  source/cli/cli_handler.cpp
  source/cli/cli_tests.cpp
  source/cli/verify_level.cpp
  source/utils_igi1conv.cpp
  source/runtime/config_qvm.cpp
  source/runtime/pause_menu_font.cpp
  source/runtime/human_player_config.cpp
  source/runtime/audio_system.cpp
  source/runtime/audio_asset_resolver.cpp
  source/renderer/object_lightmap.cpp
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

target_include_directories(igi-editor PRIVATE ${PROJECT_SOURCE_DIR}/third_party/glm-1.0.1)
target_include_directories(igi-editor PRIVATE ${PROJECT_SOURCE_DIR}/third_party/GL/include)
target_include_directories(igi-editor PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_include_directories(igi-editor PRIVATE ${PROJECT_SOURCE_DIR}/source/runtime)
target_include_directories(igi-editor PRIVATE ${PROJECT_SOURCE_DIR}/source/cli)
target_include_directories(igi-editor PRIVATE ${PROJECT_SOURCE_DIR}/third_party/tinygltf)


if (WIN32)
  # Dynamically select GL library path based on target architecture
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(GL_LIB_DIR "${PROJECT_SOURCE_DIR}/third_party/GL/lib/x64")
  else()
    set(GL_LIB_DIR "${PROJECT_SOURCE_DIR}/third_party/GL/lib/x86")
  endif()
  message("Selected GL library path: ${GL_LIB_DIR}")

  target_link_directories(igi-editor PRIVATE ${GL_LIB_DIR})
  target_link_libraries(igi-editor PRIVATE freeglut glew32 OpenGL32 winmm)

  # Increase PCH memory limit to fix compiler heap errors
  if (MSVC)
    target_compile_options(igi-editor PRIVATE "/Zm300" "/bigobj" "/FS")
  endif()

  # Set runtime library based on configuration
  set_target_properties(igi-editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin/Release"
  )

  # Ensure Release builds don't use debug runtime
  set_target_properties(igi-editor PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  )

  # Embed admin manifest
  set(MANIFEST_FILE "${PROJECT_SOURCE_DIR}/igi-editor.exe.manifest")
  if(EXISTS ${MANIFEST_FILE})
    set_target_properties(igi-editor PROPERTIES
      LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${MANIFEST_FILE} /MANIFESTUAC:\"level='asInvoker' uiAccess='false'\""
    )
  endif()

  # Use Console subsystem for both Debug and Release to support CLI mode
  set_target_properties(igi-editor PROPERTIES
    LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
  )

  # Ensure Release mode entry point is correct for console app
  set_target_properties(igi-editor PROPERTIES
    OUTPUT_NAME "igi1ed"
  )

  # Copy required runtime files (DLLs, config, version, shaders) next to the exe
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(DLL_SRC_DIR "${PROJECT_SOURCE_DIR}/assets/dlls/x64")
  else()
    set(DLL_SRC_DIR "${PROJECT_SOURCE_DIR}/assets/dlls/x86")
  endif()

  add_custom_command(TARGET igi-editor POST_BUILD
    # Copy assets/editor structure directly
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${PROJECT_SOURCE_DIR}/assets/editor"
      "$<TARGET_FILE_DIR:igi-editor>/editor"

    # Copy files to root (DLLs and version)
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${DLL_SRC_DIR}/freeglut.dll"
      "${DLL_SRC_DIR}/glew32.dll"
      "${PROJECT_SOURCE_DIR}/version"
      "$<TARGET_FILE_DIR:igi-editor>"

    COMMENT "Copying runtime files to editor/ structure"
  )
else()
  target_link_libraries(igi-editor PRIVATE glut GL GLEW)
endif()

# ─── igi1conv: prebuilt standalone asset converter ───────────────────────────
# The converter now lives in its own repo (https://github.com/heaven-hm/project-igi-conv).
# The editor consumes only the committed prebuilt binary, which the POST_BUILD
# step above copies from assets/editor/tools/ to bin/<cfg>/editor/tools/igi1conv.exe.
# Before building, fetch_igi1conv.cmake pulls the latest released igi1conv.exe
# from GitHub (falling back to the committed copy when offline/unavailable).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fetch_igi1conv.cmake)
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Force gtest to use the DLL CRT (/MD, /MDd) so it matches the project.
# Without this, gtest's internal_utils.cmake replaces /MD with /MT (static
# CRT), causing __imp__calloc_dbg / _CrtDbgReport link errors when mixing
# gtest.lib with DLL-CRT object files.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
option(FETCHCONTENT_FULLY_DISCONNECTED
    "Use already provisioned FetchContent dependencies" ON)
option(IGI_TEST_ASAN "Instrument test targets with AddressSanitizer" OFF)
FetchContent_MakeAvailable(googletest)
enable_testing()

add_executable(igi_tests
    tests/test_mcp_json.cpp
    tests/test_mcp_json_rpc.cpp
    tests/test_mcp_paths.cpp
    tests/test_mcp_transaction.cpp
    tests/test_mcp_game_snapshot.cpp
    tests/test_mcp_session_tools.cpp
    tests/test_mcp_server.cpp
    tests/test_mcp_domain_tools.cpp
    tests/test_mcp_player_tools.cpp
    tests/test_mcp_runtime_tools.cpp
    tests/test_mcp_audio_tools.cpp
    tests/test_mcp_string_tools.cpp
    tests/test_mcp_cutscene_tools.cpp
    tests/test_mcp_http.cpp
    tests/test_qsc_lexer.cpp
    tests/test_qsc_parser.cpp
    tests/test_qvm_roundtrip.cpp
    tests/test_config.cpp
    tests/test_logger.cpp
    tests/test_utils.cpp
    tests/test_verify_unit.cpp
    tests/test_verify_level.cpp
    tests/test_dat_parser.cpp
    tests/test_dat_writer.cpp
    tests/test_graph_parser.cpp
    tests/test_graph_overlay.cpp
    tests/test_animation.cpp
    tests/test_res_parser.cpp
    tests/test_res_stream_append.cpp
    tests/test_tex_parser.cpp
    tests/test_mtp_parser.cpp
    tests/test_mtp_writer.cpp
    tests/test_fnt_parser.cpp
    tests/test_pickup_resolve.cpp
    tests/test_res_model_set.cpp
    tests/test_mef_native_lightmap.cpp
    tests/test_igi1conv_lightmap.cpp
    tests/test_debug_command_parser.cpp
    tests/test_capture_camera.cpp
    tests/test_editor_camera_start.cpp
    tests/test_visual_integrity.cpp
    tests/test_runtime_subsystems.cpp
    tests/test_ai_patrol_port.cpp
    tests/test_audio_asset_resolver.cpp
    tests/test_vanilla_fixture_parity.cpp
    tests/test_mission_objectives.cpp
    tests/test_mission_expression.cpp
    tests/test_mission_state.cpp
    tests/test_level_weather.cpp
    tests/test_model_texture_resolution.cpp
    tests/test_spline_geometry.cpp
)

# Explicit runtime so igi_tests always matches igi-editor (/MDd Debug, /MD Release).
if (MSVC)
    set_target_properties(igi_tests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()

target_include_directories(igi_tests PRIVATE source)
target_include_directories(igi_tests PRIVATE ${PROJECT_SOURCE_DIR}/source/runtime)
target_include_directories(igi_tests PRIVATE ${PROJECT_SOURCE_DIR}/source/cli)
target_include_directories(igi_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/glm-1.0.1)
target_include_directories(igi_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/GL/include)
# gameplay_window.cpp uses FreeGLUT directly, so the test target needs the same
# GL library path and libs as the editor target.
if (WIN32)
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TESTS_GL_LIB_DIR "${PROJECT_SOURCE_DIR}/third_party/GL/lib/x64")
  else()
    set(TESTS_GL_LIB_DIR "${PROJECT_SOURCE_DIR}/third_party/GL/lib/x86")
  endif()
  target_link_directories(igi_tests PRIVATE ${TESTS_GL_LIB_DIR})
  target_link_libraries(igi_tests PRIVATE gtest_main winmm ws2_32 bcrypt freeglut glew32 OpenGL32 gdiplus psapi wbemuuid ole32)
else()
  target_link_libraries(igi_tests PRIVATE gtest_main winmm)
endif()
target_sources(igi_tests PRIVATE
    source/mcp/mcp_json.cpp
    source/mcp/mcp_json_rpc.cpp
    source/mcp/game_data_service.cpp
    source/mcp/mcp_transaction.cpp
    source/mcp/mcp_tools_session.cpp
    source/mcp/mcp_server.cpp
    source/mcp/mcp_transport_stdio.cpp
    source/mcp/mcp_transport_http.cpp
    source/mcp/mcp_tools_objects.cpp
    source/mcp/mcp_tools_ai.cpp
    source/mcp/mcp_tools_player.cpp
    source/mcp/mcp_tools_runtime.cpp
    source/mcp/mcp_tools_audio.cpp
    source/mcp/mcp_tools_strings.cpp
    source/mcp/mcp_tools_cutscene.cpp
    source/mcp/mcp_tools_mission.cpp
    source/mcp/mcp_tools_graph.cpp
    source/mcp/mcp_tools_assets.cpp
    source/level/qsc_lexer.cpp
    source/level/qsc_parser.cpp
    source/level/qvm_parser.cpp
    source/level/qvm_ai_bindings.cpp
    source/level/qvm_compiler.cpp
    source/level/qvm_decompiler.cpp
    source/level/level_common.cpp
    source/config.cpp
    source/utils.cpp
    source/common.cpp
    source/logger.cpp
    source/level/task_schema.cpp
    source/level/level_objects.cpp
    source/level/level_objects_serialize.cpp
    source/cli/verify_level_core.cpp
    source/cli/verify_level.cpp
    source/renderer/dat_writer.cpp
    source/renderer/graph_writer.cpp
    source/renderer/graph_project.cpp
    source/renderer/res_writer.cpp
    source/renderer/res_compiler.cpp
    source/level/res_model_set.cpp
    source/renderer/tex_writer.cpp
    source/renderer/model_texture_resolution.cpp
    source/renderer/spline_geometry.cpp
    source/level/mtp_writer.cpp
    source/renderer/fnt_parser.cpp
    source/level/terrain_files.cpp
    source/renderer/mef_native.cpp
    source/visual_integrity.cpp
    source/utils_igi1conv.cpp
    source/game_clock.cpp
    source/level/qvm_native_registry.cpp
    source/level/qvm_interpreter.cpp
    source/level/task_tree.cpp
    source/player_collision.cpp
    source/player_fall_impact.cpp
    source/player_motion.cpp
    source/player_ladder.cpp
    source/player_separation.cpp
    source/animation_motion.cpp
    source/player_animation_driver.cpp
    source/animation.cpp
    source/weapon_view_sway.cpp
    source/weapon_view_recoil.cpp
    source/player_controller.cpp
    source/weapon_system.cpp
    source/ai_events.cpp
    source/ai_script_host.cpp
    source/ai_system.cpp
    source/level_flow.cpp
    source/mission_objective_loader.cpp
    source/mission_flow_loader.cpp
    source/mission_state_loader.cpp
    source/mission_expression.cpp
    source/runtime/runtime_world.cpp
    source/runtime/runtime_session.cpp
    source/runtime/gameplay_window.cpp
    source/runtime/editor_snapshot.cpp
    source/runtime/window_input_router.cpp
    source/runtime/simulation_scheduler.cpp
    source/runtime/gameplay_host.cpp
    source/runtime/gameplay_spawn.cpp
    source/runtime/runtime_renderer.cpp
    source/runtime/projectile_system.cpp
    source/runtime/door_state.cpp
    source/runtime/audio_system.cpp
    source/runtime/audio_asset_resolver.cpp
    source/runtime/config_qvm.cpp
    source/runtime/pause_menu_font.cpp
    source/runtime/human_player_config.cpp
    source/runtime/magic_object_registry.cpp
    source/runtime/map_computer_camera.cpp
    source/runtime/map_computer_projection.cpp
    source/runtime/level_weather.cpp
)

set_source_files_properties(
    source/runtime/audio_system.cpp
    source/runtime/audio_asset_resolver.cpp
    source/runtime/config_qvm.cpp
    source/runtime/human_player_config.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

# level_objects.cpp (and its pch.h header graph: app.h, renderer, level.h) relies
# on pch.h being force-included to establish the correct include order. Apply the
# same precompiled header here so it compiles identically to the main target.
target_precompile_headers(igi_tests PRIVATE source/pch.h)

include(GoogleTest)
gtest_discover_tests(igi_tests
    TEST_PREFIX "aggregate."
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# Keep the editor-start policy regression lightweight and independent from the
# large runtime integration translation unit/PCH. This gives the camera policy
# a fast, reliable unit-test seam even when the full aggregate test target is
# memory constrained on Windows.
add_executable(editor_camera_start_tests
    tests/test_editor_camera_start.cpp
)

if(IGI_TEST_ASAN AND MSVC)
    # ASan is an isolated opt-in lane. Release symbols avoid /RTC conflicts;
    # disable incremental linking because the linker does not support it with
    # the sanitizer runtime.
    target_compile_options(igi_tests PRIVATE /fsanitize=address /Zi)
    target_link_options(igi_tests PRIVATE /INCREMENTAL:NO)
endif()
target_include_directories(editor_camera_start_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/source
    ${PROJECT_SOURCE_DIR}/third_party/glm-1.0.1
    ${PROJECT_SOURCE_DIR}/third_party/GL/include
)
target_link_libraries(editor_camera_start_tests PRIVATE gtest_main)
target_precompile_headers(editor_camera_start_tests PRIVATE source/pch.h)
gtest_discover_tests(editor_camera_start_tests
    TEST_PREFIX "camera."
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(editor_policy_tests
    tests/test_editor_policy.cpp
)
target_include_directories(editor_policy_tests PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(editor_policy_tests PRIVATE gtest_main)
gtest_discover_tests(editor_policy_tests
    TEST_PREFIX "policy."
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(game_clock_tests
    tests/test_game_clock.cpp
    source/game_clock.cpp
)
target_include_directories(game_clock_tests PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(game_clock_tests PRIVATE gtest_main)
gtest_discover_tests(game_clock_tests
    TEST_PREFIX "clock."
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(debug_command_queue_tests
    tests/test_debug_command_queue.cpp
)
target_include_directories(debug_command_queue_tests PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(debug_command_queue_tests PRIVATE gtest_main)
gtest_discover_tests(debug_command_queue_tests
    TEST_PREFIX "debug_queue."
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_executable(igi_mcp
    source/mcp/mcp_main.cpp
    source/mcp/mcp_server.cpp
    source/mcp/mcp_transport_stdio.cpp
    source/mcp/mcp_transport_http.cpp
    source/mcp/mcp_json.cpp
    source/mcp/mcp_json_rpc.cpp
    source/mcp/game_data_service.cpp
    source/mcp/mcp_transaction.cpp
    source/mcp/mcp_tools_session.cpp
    source/mcp/mcp_tools_objects.cpp
    source/mcp/mcp_tools_ai.cpp
    source/mcp/mcp_tools_player.cpp
    source/mcp/mcp_tools_runtime.cpp
    source/mcp/mcp_tools_audio.cpp
    source/mcp/mcp_tools_strings.cpp
    source/mcp/mcp_tools_cutscene.cpp
    source/mcp/mcp_tools_mission.cpp
    source/mcp/mcp_tools_graph.cpp
    source/mcp/mcp_tools_assets.cpp
    source/mcp/mcp_logger_stub.cpp
    source/level/qsc_lexer.cpp
    source/level/qsc_parser.cpp
    source/level/qvm_parser.cpp
    source/level/qvm_compiler.cpp
    source/level/qvm_decompiler.cpp
    source/level/task_schema.cpp
    source/renderer/res_writer.cpp
)
target_include_directories(igi_mcp PRIVATE source)
if (WIN32)
    target_link_libraries(igi_mcp PRIVATE ws2_32 bcrypt gdiplus psapi wbemuuid ole32)
    set_target_properties(igi_mcp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/bin/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin/Release"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Copy test fixtures next to igi_tests.exe so tests work from any directory
add_custom_command(TARGET igi_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/tests/fixtures"
        "$<TARGET_FILE_DIR:igi_tests>/fixtures"
    COMMENT "Copying test fixtures to output directory"
)
