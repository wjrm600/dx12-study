// =====================================================================
//  DX12 학습 실습 앱 (DX12 Study Lab)
//
//  공부한 주제마다 "장면"을 하나씩 만들어 이 앱 안에 차곡차곡 쌓는다.
//  오른쪽 패널 맨 위의 "장면" 메뉴에서 고른다.
//
//    장면 1: 래스터라이저 - 삼각형이 어떤 픽셀을 칠하는지 (무게중심 wa·wb·wc, 외적 넓이, 컬링)
//    (다음 예정) 큐브와 깊이 버퍼, 텍스처, 루트 시그니처 ...
//
//  파일 구성:
//    core.h / core.cpp   : 모든 장면이 같이 쓰는 DX12 뼈대 (장치, 스왑체인, 펜스 ...)
//    scene.h             : 장면이 채워야 하는 함수 목록
//    shader_file.*       : .hlsl 파일 읽기, 저장하면 다시 컴파일(핫 리로드), 셰이더 편집 창
//    scenes/scene_*.cpp  : 장면 하나 = 주제 하나
//    ../shaders/*.hlsl   : 장면마다 셰이더 파일 하나 (앱이 켜져 있어도 고치면 바로 반영)
//    main.cpp (이 파일)   : 창, ImGui, 장면 메뉴, 메인 루프
// =====================================================================
#include "core.h"
#include "scene.h"
#include "shader_file.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------
// 장면 목록: 새 장면을 만들면 여기에 한 줄 추가
// ---------------------------------------------------------------------
using SceneFactory = Scene* (*)();
static const SceneFactory kScenes[] = {
    CreateRasterScene,      // 장면 1: 래스터라이저
};
static const int kSceneCount = (int)(sizeof(kScenes) / sizeof(kScenes[0]));

static std::unique_ptr<Scene> g_scene;
static int   g_sceneIndex = -1;
static float g_dpi = 1.0f;
static float g_panelWidth = 460.0f;   // DPI 곱하기 전 값
static bool  g_resizePending = false;
static UINT  g_newW = 0, g_newH = 0;

static void SwitchScene(int index)
{
    if (index == g_sceneIndex) return;
    WaitIdle();                      // GPU가 옛 장면 자원을 다 쓴 뒤에 지운다
    if (g_scene) g_scene->Shutdown();
    g_scene.reset(kScenes[index]());
    g_scene->Init();
    g_sceneIndex = index;
}

// ---------------------------------------------------------------------
// 오른쪽 패널
// ---------------------------------------------------------------------
static void DrawPanel(float panelW)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelW, 0));
    ImGui::SetNextWindowSize(ImVec2(panelW, io.DisplaySize.y));
    ImGui::Begin("패널", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "DX12 학습 실습 앱");
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f FPS", io.Framerate);
    ImGui::TextDisabled("%s", g_dx.adapterName.c_str());

    // 장면 고르기
    ImGui::SetNextItemWidth(-1);
    const char* cur = g_scene ? g_scene->Name() : "";
    if (ImGui::BeginCombo("##scene", cur))
    {
        for (int i = 0; i < kSceneCount; i++)
        {
            std::unique_ptr<Scene> tmp(kScenes[i]());   // 이름만 보려고 잠깐 만든다 (GPU 자원은 Init 전이라 없음)
            if (ImGui::Selectable(tmp->Name(), i == g_sceneIndex)) SwitchScene(i);
        }
        ImGui::Separator();
        ImGui::TextDisabled("다음 예정: 큐브와 깊이 버퍼, 텍스처, 루트 시그니처");
        ImGui::EndCombo();
    }
    ImGui::Checkbox("VSync (모니터 주사율에 맞추기)", &g_dx.vsync);
    ImGui::Separator();

    if (g_scene) g_scene->OnPanel();
    ImGui::End();
}

// ---------------------------------------------------------------------
// 창
// ---------------------------------------------------------------------
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && g_dx.device)
        {
            g_newW = LOWORD(lParam); g_newH = HIWORD(lParam);
            if (g_newW != g_dx.width || g_newH != g_dx.height) g_resizePending = true;
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"DX12StudyLab", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"DX12 학습 실습 앱", WS_OVERLAPPEDWINDOW, 60, 60,
                              (int)(1500 * g_dpi), (int)(900 * g_dpi), nullptr, nullptr, hInst, nullptr);
    InitD3D(hwnd);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(g_dpi);
    style.FontScaleDpi = g_dpi;
    style.FontSizeBase = 17.0f;
    style.WindowRounding = 0.0f;
    const char* kFont = "C:\\Windows\\Fonts\\malgun.ttf";   // 한글: 맑은 고딕
    if (GetFileAttributesA(kFont) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(kFont))
        io.Fonts->AddFontDefault();
    // 셰이더 편집 창용 고정폭 글꼴: Consolas + 한글은 맑은 고딕으로 채움
    const char* kMono = "C:\\Windows\\Fonts\\consola.ttf";
    if (GetFileAttributesA(kMono) != INVALID_FILE_ATTRIBUTES)
    {
        g_codeFont = io.Fonts->AddFontFromFileTTF(kMono);
        if (g_codeFont && GetFileAttributesA(kFont) != INVALID_FILE_ATTRIBUTES)
        {
            ImFontConfig merge; merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(kFont, 0.0f, &merge);
        }
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g_dx.device.Get();
    ii.CommandQueue = g_dx.queue.Get();
    ii.NumFramesInFlight = kFrameCount;
    ii.RTVFormat = kBackBufferFormat;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_dx.srvHeap.Get();
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) { AllocSrv(c, g); };
    ii.SrvDescriptorFreeFn  = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE) { FreeSrv(c); };
    ImGui_ImplDX12_Init(&ii);

    SwitchScene(0);

    // 개발용 자동 테스트: LAB_AUTOTEST=1 이면 장면마다 AutoTest 를 돌리고 결과를 남긴 뒤 끝낸다
    const bool autotest = GetEnvironmentVariableA("LAB_AUTOTEST", nullptr, 0) > 0;
    int testFrame = 0, failures = 0;
    if (autotest) g_dx.vsync = false;

    const float clear[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) { Sleep(16); continue; }
        if (g_resizePending) { g_resizePending = false; ResizeSwapChain(g_newW, g_newH); }

        // 1) 이 프레임 자원 세트가 비었으면 장면이 되읽기 결과를 꺼낸다
        WaitCurrentFrameSlot();
        g_scene->OnFrameReused(g_dx.frameIndex);

        // 2) UI
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        float panelW = std::min(g_panelWidth * g_dpi, io.DisplaySize.x * 0.6f);
        Canvas canvas{ 0, 0, std::max(1.0f, io.DisplaySize.x - panelW), std::max(1.0f, io.DisplaySize.y) };
        DrawPanel(panelW);
        g_scene->OnCanvas(canvas);
        ImGui::Render();

        // 3) GPU 명령 기록 -> 4) ImGui 덮어 그리기 + Present
        ID3D12GraphicsCommandList* cl = BeginFrame(clear);
        g_scene->OnRender(cl, canvas);
        EndFrame();

        if (autotest)
        {
            if (g_scene->AutoTest(testFrame++, failures) || testFrame > 5000)
            {
                char line[128]; snprintf(line, sizeof line, "LAB_AUTOTEST %s failures=%d frames=%d\n", failures ? "FAIL" : "PASS", failures, testFrame);
                FILE* f = fopen("lab_autotest.log", "a"); if (f) { fputs(line, f); fclose(f); }
                OutputDebugStringA(line);
                done = true;
            }
        }
    }

    WaitIdle();
    if (g_scene) g_scene->Shutdown();
    g_scene.reset();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ShutdownD3D();
    return failures ? 2 : 0;
}
