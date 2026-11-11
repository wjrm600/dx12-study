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
    src/shader_file.cpp
    src/shader_file.h
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
# _CRT_SECURE_NO_WARNINGS: fopen / _wfopen 을 그대로 쓴다 (MSVC 의 "fopen_s 를 쓰라"는 C4996 경고 끄기)
target_compile_definitions(DX12StudyLab PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(DX12StudyLab PRIVATE /utf-8 /W3 /MP)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DX12StudyLab)
    source_group("scenes" FILES ${SCENE_SOURCES})
endif()
target_link_libraries(DX12StudyLab PRIVATE d3d12 dxgi d3dcompiler dxguid shell32)

# 셰이더(.hlsl)는 빌드에 넣지 않고 실행 중에 읽는다(고치면 바로 반영).
#  - 앱은 먼저 이 소스 폴더의 shaders 를 읽는다 -> 고친 내용이 git 저장소에 그대로 남음
#  - exe 옆에도 복사해 둔다 -> exe 만 다른 곳으로 옮겨도 실행됨
target_compile_definitions(DX12StudyLab PRIVATE LAB_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/")
file(GLOB SHADER_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.hlsl)
target_sources(DX12StudyLab PRIVATE ${SHADER_FILES})
set_source_files_properties(${SHADER_FILES} PROPERTIES VS_TOOL_OVERRIDE "None")   # VS 가 .hlsl 을 따로 컴파일하지 않게
if(MSVC)
    source_group("shaders" FILES ${SHADER_FILES})
endif()
add_custom_command(TARGET DX12StudyLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:DX12StudyLab>/shaders)
