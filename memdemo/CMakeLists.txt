cmake_minimum_required(VERSION 3.20)
project(DX12MemoryDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# C++ 런타임을 exe 안에 넣는다(/MT). 받는 PC에 "Visual C++ 재배포 패키지"가 없어도 실행된다.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Agility SDK: 최신 D3D12 런타임을 exe 옆에 같이 둔다 (GPU Upload Heap / ReBAR 기능용).
# 인터넷이 안 되면 -DUSE_AGILITY_SDK=OFF 로 꺼도 나머지는 전부 동작한다.
option(USE_AGILITY_SDK "Download the D3D12 Agility SDK from nuget.org" ON)
set(AGILITY_VERSION "1.619.6")
set(AGILITY_SDK_NUMBER 619)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_executable(DX12MemoryDemo WIN32
    src/main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(DX12MemoryDemo PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(DX12MemoryDemo PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(DX12MemoryDemo PRIVATE /utf-8 /W3 /MP)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DX12MemoryDemo)
endif()
target_link_libraries(DX12MemoryDemo PRIVATE d3d12 dxgi d3dcompiler dxguid psapi dwmapi)

if(USE_AGILITY_SDK)
    set(AG_DIR ${CMAKE_BINARY_DIR}/agility-${AGILITY_VERSION})
    set(AG_ZIP ${CMAKE_BINARY_DIR}/agility-${AGILITY_VERSION}.zip)
    if(NOT EXISTS ${AG_DIR}/build/native/bin/x64/D3D12Core.dll)
        message(STATUS "Downloading D3D12 Agility SDK ${AGILITY_VERSION} ...")
        file(DOWNLOAD https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${AGILITY_VERSION} ${AG_ZIP} STATUS AG_STATUS)
        list(GET AG_STATUS 0 AG_CODE)
        if(AG_CODE EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT ${AG_ZIP} DESTINATION ${AG_DIR})
        endif()
    endif()
    if(EXISTS ${AG_DIR}/build/native/bin/x64/D3D12Core.dll)
        target_compile_definitions(DX12MemoryDemo PRIVATE DEMO_AGILITY_SDK_VERSION=${AGILITY_SDK_NUMBER})
        add_custom_command(TARGET DX12MemoryDemo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:DX12MemoryDemo>/D3D12
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AG_DIR}/build/native/bin/x64/D3D12Core.dll $<TARGET_FILE_DIR:DX12MemoryDemo>/D3D12/
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AG_DIR}/build/native/bin/x64/d3d12SDKLayers.dll $<TARGET_FILE_DIR:DX12MemoryDemo>/D3D12/)
        message(STATUS "Agility SDK ${AGILITY_VERSION}: ON")
    else()
        message(WARNING "Agility SDK를 받지 못했습니다. GPU Upload Heap(ReBAR) 항목은 OS 기본 런타임이 지원할 때만 켜집니다. 나머지는 정상 동작합니다.")
    endif()
endif()
