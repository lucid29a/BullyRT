add_library(BullyDX12_PostFX SHARED PostFX.cpp)
set_target_properties(BullyDX12_PostFX PROPERTIES OUTPUT_NAME PostFX)
target_include_directories(BullyDX12_PostFX PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(BullyDX12_PostFX PRIVATE UNICODE _UNICODE)
target_compile_options(BullyDX12_PostFX PRIVATE ${BDX12_COMMON_FLAGS} /EHsc)
target_link_libraries(BullyDX12_PostFX PRIVATE d3d12)

bdx12_compile_shader(BullyDX12_PostFX ${CMAKE_CURRENT_SOURCE_DIR}/PostFX.hlsl VSMain vs_5_1 g_PostFX_VSMain)
bdx12_compile_shader(BullyDX12_PostFX ${CMAKE_CURRENT_SOURCE_DIR}/PostFX.hlsl PSMain ps_5_1 g_PostFX_PSMain)

if(BULLYDX12_DEPLOY)
    add_custom_command(TARGET BullyDX12_PostFX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BULLY_GAME_DIR}/BullyDX12/extensions"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:BullyDX12_PostFX> "${BULLY_GAME_DIR}/BullyDX12/extensions/PostFX.dll"
        COMMENT "Deploying PostFX.dll")
endif()
