add_library(BullyDX12_RaytracingFoundation SHARED RaytracingFoundation.cpp)
set_target_properties(BullyDX12_RaytracingFoundation PROPERTIES OUTPUT_NAME RaytracingFoundation)
target_include_directories(BullyDX12_RaytracingFoundation PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(BullyDX12_RaytracingFoundation PRIVATE UNICODE _UNICODE)
target_compile_options(BullyDX12_RaytracingFoundation PRIVATE ${BDX12_COMMON_FLAGS} /EHsc)
target_link_libraries(BullyDX12_RaytracingFoundation PRIVATE d3d12)

if(BULLYDX12_DEPLOY)
    add_custom_command(TARGET BullyDX12_RaytracingFoundation POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BULLY_GAME_DIR}/BullyDX12/extensions"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:BullyDX12_RaytracingFoundation> "${BULLY_GAME_DIR}/BullyDX12/extensions/RaytracingFoundation.dll"
        COMMENT "Deploying RaytracingFoundation.dll")
endif()
