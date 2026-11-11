add_library(BullyDX12_RaytracingBridge SHARED RaytracingBridge.cpp ShaderPatcher.cpp GameCulling.cpp DrawDistance.cpp Pools.cpp GameLights.cpp Menu.cpp RtCycle.cpp GameErrors.cpp
    ${PROJECT_SOURCE_DIR}/external/imgui/imgui.cpp ${PROJECT_SOURCE_DIR}/external/imgui/imgui_draw.cpp
    ${PROJECT_SOURCE_DIR}/external/imgui/imgui_tables.cpp ${PROJECT_SOURCE_DIR}/external/imgui/imgui_widgets.cpp
    ${PROJECT_SOURCE_DIR}/external/imgui/backends/imgui_impl_dx9.cpp ${PROJECT_SOURCE_DIR}/external/imgui/backends/imgui_impl_win32.cpp)
set_target_properties(BullyDX12_RaytracingBridge PROPERTIES OUTPUT_NAME RaytracingBridge)
target_include_directories(BullyDX12_RaytracingBridge PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/external/imgui)
target_compile_definitions(BullyDX12_RaytracingBridge PRIVATE UNICODE _UNICODE)
target_compile_options(BullyDX12_RaytracingBridge PRIVATE ${BDX12_COMMON_FLAGS} /EHsc /utf-8)
target_link_libraries(BullyDX12_RaytracingBridge PRIVATE d3d12)

bdx12_compile_shader(BullyDX12_RaytracingBridge ${CMAKE_CURRENT_SOURCE_DIR}/Composite.hlsl VSMain vs_5_1 g_RTB_VSMain)
bdx12_compile_shader(BullyDX12_RaytracingBridge ${CMAKE_CURRENT_SOURCE_DIR}/Composite.hlsl PSDepth ps_5_1 g_RTB_PSDepth)
bdx12_compile_shader(BullyDX12_RaytracingBridge ${CMAKE_CURRENT_SOURCE_DIR}/Composite.hlsl PSComposite ps_5_1 g_RTB_PSComposite)
bdx12_compile_shader(BullyDX12_RaytracingBridge ${CMAKE_CURRENT_SOURCE_DIR}/Composite.hlsl PSVis ps_5_1 g_RTB_PSVis)

if(BULLYDX12_DEPLOY)
    add_custom_command(TARGET BullyDX12_RaytracingBridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BULLY_GAME_DIR}/BullyDX12/extensions"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:BullyDX12_RaytracingBridge> "${BULLY_GAME_DIR}/BullyDX12/extensions/RaytracingBridge.dll"
        COMMENT "Deploying RaytracingBridge.dll")
endif()
