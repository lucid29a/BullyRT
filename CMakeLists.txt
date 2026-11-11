cmake_minimum_required(VERSION 3.20)
project(BullyDX12 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Bully.exe is a 32-bit process: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

set(BULLY_GAME_DIR "" CACHE PATH "Bully folder (the one with Bully.exe): the build is copied there when set")
if(BULLY_GAME_DIR)
    set(BULLYDX12_DEPLOY ON)
else()
    set(BULLYDX12_DEPLOY OFF)
endif()

get_filename_component(_sdk_bin "$ENV{WindowsSdkVerBinPath}" ABSOLUTE)
find_program(FXC_EXECUTABLE fxc HINTS "${_sdk_bin}/x64" "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(NOT FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc.exe not found (Windows SDK)")
endif()

function(bdx12_compile_shader target source entry profile varname)
    get_filename_component(_name ${source} NAME_WE)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_name}_${entry}.h")
    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${FXC_EXECUTABLE} /nologo /O3 /WX /T ${profile} /E ${entry} /Vn ${varname} /Fh ${_out} ${source}
        MAIN_DEPENDENCY ${source}
        COMMENT "fxc ${profile} ${_name}:${entry}"
        VERBATIM)
    target_sources(${target} PRIVATE ${_out})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
endfunction()

set(BDX12_COMMON_FLAGS /W4 /permissive- /utf-8 /MP /Zi /wd4100 /wd4324)
add_link_options(/DEBUG $<$<NOT:$<CONFIG:Debug>>:/OPT:REF> $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)

add_library(BullyDX12 SHARED
    src/dllmain.cpp
    src/Common.cpp
    src/Config.cpp
    src/D3D12Context.cpp
    src/Backend.cpp
    src/ExtensionManager.cpp
    src/ShaderOverride.cpp
    src/FramePipeline.cpp
    src/Display.cpp
    src/Telemetry.cpp
    src/Proxy/Direct3D9Proxy.cpp
    src/Proxy/Device9Proxy.cpp
    src/d3d9.def
    src/version.rc)
set_target_properties(BullyDX12 PROPERTIES OUTPUT_NAME d3d9)
target_include_directories(BullyDX12 PUBLIC include)
target_compile_definitions(BullyDX12 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(BullyDX12 PRIVATE ${BDX12_COMMON_FLAGS} /EHsc)
target_link_libraries(BullyDX12 PRIVATE d3d12 dxgi dxguid)
target_link_options(BullyDX12 PRIVATE /SAFESEH:NO /ENTRY:BDX_EntryPoint@12)

add_subdirectory(extensions/PostFX)
add_subdirectory(extensions/RaytracingFoundation)
add_subdirectory(extensions/RaytracingBridge)

if(BULLYDX12_DEPLOY)
    add_custom_command(TARGET BullyDX12 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:BullyDX12> "${BULLY_GAME_DIR}/d3d9.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:BullyDX12> "${BULLY_GAME_DIR}/d3d9.pdb"
        COMMENT "Deploying d3d9.dll to ${BULLY_GAME_DIR}")
endif()

option(BULLYDX12_BUILD_TESTS "Build the integration test harness" ON)
if(BULLYDX12_BUILD_TESTS)
    add_executable(BullyDX12_TestHarness tests/TestHarness.cpp)
    target_compile_definitions(BullyDX12_TestHarness PRIVATE UNICODE _UNICODE)
    target_compile_options(BullyDX12_TestHarness PRIVATE ${BDX12_COMMON_FLAGS} /EHsc)
    target_link_libraries(BullyDX12_TestHarness PRIVATE user32)
    bdx12_compile_shader(BullyDX12_TestHarness ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestShaders.hlsl VSMain vs_3_0 g_TestVS)
    bdx12_compile_shader(BullyDX12_TestHarness ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestShaders.hlsl PSMain ps_3_0 g_TestPS)
endif()
if(BULLYDX12_BUILD_TESTS)
    add_executable(BullyDX12_TestRT tests/TestRT.cpp)
    target_compile_definitions(BullyDX12_TestRT PRIVATE UNICODE _UNICODE)
    target_compile_options(BullyDX12_TestRT PRIVATE ${BDX12_COMMON_FLAGS} /EHsc)
    target_link_libraries(BullyDX12_TestRT PRIVATE user32)
    foreach(_e VSScene VSHud VSSceneW VSNoCam)
        bdx12_compile_shader(BullyDX12_TestRT ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestRTShaders.hlsl ${_e} vs_3_0 g_${_e})
    endforeach()
    foreach(_e VSSkin VSAlpha)
        bdx12_compile_shader(BullyDX12_TestRT ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestSkin.hlsl ${_e} vs_3_0 g_${_e})
    endforeach()
    foreach(_e PSSkin PSAlpha PSAlphaClip PSGlass)
        bdx12_compile_shader(BullyDX12_TestRT ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestSkin.hlsl ${_e} ps_3_0 g_${_e})
    endforeach()
    foreach(_e PSScene PSHud PSShadowRecv PSPointLit)
        bdx12_compile_shader(BullyDX12_TestRT ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestRTShaders.hlsl ${_e} ps_3_0 g_${_e})
    endforeach()
endif()

if(BULLYDX12_BUILD_TESTS)
    add_executable(BullyDX12_Replay tests/Replay.cpp)
    target_compile_definitions(BullyDX12_Replay PRIVATE UNICODE _UNICODE)
    target_compile_options(BullyDX12_Replay PRIVATE ${BDX12_COMMON_FLAGS} /EHsc)
    target_link_libraries(BullyDX12_Replay PRIVATE user32)
    target_include_directories(BullyDX12_Replay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    add_dependencies(BullyDX12_Replay BullyDX12_TestRT)
endif()
