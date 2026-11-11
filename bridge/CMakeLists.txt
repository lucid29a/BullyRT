# BullyRTBridge - 64-bit ray tracing process (separate CMake tree: configure with -A x64)

cmake_minimum_required(VERSION 3.20)
project(BullyRTBridge LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BullyRTBridge must be built for x64 (-A x64): DXR is not available to 32-bit processes")
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

find_program(DXC_EXECUTABLE dxc HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "dxc.exe not found (Windows SDK)")
endif()

set(_ext "${CMAKE_CURRENT_SOURCE_DIR}/../external")
set(NRI_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12_SUPPORT ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_VK_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NONE_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_WGPU_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NVAPI OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_AMDAGS OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_AGILITY_SDK_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_IMGUI_EXTENSION OFF CACHE BOOL "" FORCE)
add_subdirectory("${_ext}/NRI" "${CMAKE_CURRENT_BINARY_DIR}/NRI")
target_compile_definitions(NRI_D3D12 PRIVATE D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT=128)

set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRD_NRI OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
add_subdirectory("${_ext}/NRD" "${CMAKE_CURRENT_BINARY_DIR}/NRD")

set(_shader_headers)
foreach(_entry CSSharcUpdate CSSharcResolve CSTrace CSVolRestir CSFroxel CSAirIntegrate CSComposite CSExposure CSNoisy)
    set(_h "${CMAKE_CURRENT_BINARY_DIR}/shaders/RTX_${_entry}.h")
    if(_entry STREQUAL "CSSharcUpdate")
        set(_sharc -D SHARC_UPDATE=1 -D SHARC_QUERY=0)
    elseif(_entry STREQUAL "CSSharcResolve")
        set(_sharc -D SHARC_UPDATE=0 -D SHARC_QUERY=0)
    else()
        set(_sharc -D SHARC_UPDATE=0 -D SHARC_QUERY=1)
    endif()
    if(_entry STREQUAL "CSTrace")
        list(APPEND _sharc -D RESTIR=1)
    endif()
    add_custom_command(
        OUTPUT ${_h}
        COMMAND ${DXC_EXECUTABLE} -nologo -O3 -T cs_6_6 -enable-16bit-types ${_sharc} -E ${_entry} -Vn g_RTX_${_entry} -I "${_ext}/NRD/Shaders" -I "${_ext}/RTXGI/Libraries/Sharc/include" -I "${_ext}/RTXDI/Include" -I "${CMAKE_CURRENT_SOURCE_DIR}" -Fh ${_h} ${CMAKE_CURRENT_SOURCE_DIR}/RTX.hlsl
        MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/RTX.hlsl
        DEPENDS "${_ext}/NRD/Shaders/NRD.hlsli" "${_ext}/RTXGI/Libraries/Sharc/include/SharcCommon.h" ${CMAKE_CURRENT_SOURCE_DIR}/RtxdiBridge.hlsli
        COMMENT "dxc cs_6_6 RTX.hlsl:${_entry}"
        VERBATIM)
    list(APPEND _shader_headers ${_h})
endforeach()

add_executable(BullyRTBridge WIN32 BullyRTBridge.cpp ${_shader_headers})
target_include_directories(BullyRTBridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(BullyRTBridge PRIVATE UNICODE _UNICODE)
target_compile_options(BullyRTBridge PRIVATE /W4 /permissive- /utf-8 /Zi /EHsc /wd4100)
target_link_options(BullyRTBridge PRIVATE /DEBUG $<$<NOT:$<CONFIG:Debug>>:/OPT:REF> $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)
target_link_libraries(BullyRTBridge PRIVATE d3d12 dxgi shell32 NRI NRD "${_ext}/DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
target_include_directories(BullyRTBridge PRIVATE "${_ext}/DLSS/include")
target_include_directories(BullyRTBridge PRIVATE "${_ext}/NRD/Integration" "${_ext}/NRD/Include" "${_ext}/NRI/Include")

set(AGILITY_VERSION 1.620.2)
set(_agility "${_ext}/AgilitySDK/${AGILITY_VERSION}")
set(_d3d12core "${_agility}/build/native/bin/x64/D3D12Core.dll")
if(NOT EXISTS "${_d3d12core}")
    set(_pkg "${_ext}/AgilitySDK/microsoft.direct3d.d3d12.${AGILITY_VERSION}.nupkg")
    if(NOT EXISTS "${_pkg}")
        message(STATUS "Downloading the DirectX 12 Agility SDK ${AGILITY_VERSION}")
        file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${AGILITY_VERSION}/microsoft.direct3d.d3d12.${AGILITY_VERSION}.nupkg"
             "${_pkg}" EXPECTED_HASH SHA256=70489669c2bbeaf318b62d72b0ac3e753d521387bfc848c9763791c4e920bfcc TLS_VERIFY ON)
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_pkg}" DESTINATION "${_agility}")
endif()
add_custom_command(TARGET BullyRTBridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BullyRTBridge>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_d3d12core}" "$<TARGET_FILE_DIR:BullyRTBridge>/D3D12/D3D12Core.dll")

if(BULLYDX12_DEPLOY)
    add_custom_command(TARGET BullyRTBridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BULLY_GAME_DIR}/BullyDX12/bridge/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_d3d12core}" "${BULLY_GAME_DIR}/BullyDX12/bridge/D3D12/D3D12Core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/materials.ini "${BULLY_GAME_DIR}/BullyDX12/bridge/materials.ini"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ext}/DLSS/lib/Windows_x86_64/rel/nvngx_dlssd.dll" "${BULLY_GAME_DIR}/BullyDX12/bridge/nvngx_dlssd.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:BullyRTBridge> "${BULLY_GAME_DIR}/BullyDX12/bridge/BullyRTBridge.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:BullyRTBridge> "${BULLY_GAME_DIR}/BullyDX12/bridge/BullyRTBridge.pdb"
        COMMENT "Deploying BullyRTBridge.exe")
endif()
