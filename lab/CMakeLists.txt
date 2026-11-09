cmake_minimum_required(VERSION 3.20)
project(DX12StudyLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# C++ 런타임을 exe 안에 넣는다(/MT). 받는 PC에 "Visual C++ 재배포 패키지"가 없어도 실행된다.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

# 장면 파일은 scenes 폴더에 넣기만 하면 자동으로 빌드에 들어간다
file(GLOB SCENE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/scenes/*.cpp)

add_executable(DX12StudyLab WIN32
    src/main.cpp
    src/core.cpp
    src/core.h
    src/scene.h
    ${SCENE_SOURCES}
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(DX12StudyLab PRIVATE src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(DX12StudyLab PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(DX12StudyLab PRIVATE /utf-8 /W3 /MP)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DX12StudyLab)
    source_group("scenes" FILES ${SCENE_SOURCES})
endif()
target_link_libraries(DX12StudyLab PRIVATE d3d12 dxgi d3dcompiler dxguid)
