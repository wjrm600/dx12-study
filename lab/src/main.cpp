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
    // 멀티 뷰포트를 켜면 ImGui 좌표는 "모니터 기준"이라, 메인 창의 위치(vp->Pos)를 더해서 놓는다
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - panelW, vp->Pos.y));
    ImGui::SetNextWindowSize(ImVec2(panelW, vp->Size.y));
    ImGui::SetNextWindowViewport(vp->ID);
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

static HWND CreateMainWindow(HINSTANCE hInst)
{
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"DX12StudyLab", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"DX12 학습 실습 앱", WS_OVERLAPPEDWINDOW, 60, 60,
                              (int)(1500 * g_dpi), (int)(900 * g_dpi), nullptr, nullptr, hInst, nullptr);
    return hwnd;
}

// 글꼴: 한글은 맑은 고딕, 셰이더 창은 Consolas + 한글은 맑은 고딕으로 채움
static void LoadFonts(ImGuiIO& io)
{
    const char* kFont = "C:\\Windows\\Fonts\\malgun.ttf";
    if (GetFileAttributesA(kFont) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(kFont))
        io.Fonts->AddFontDefault();
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
}

static void InitImGui(HWND hwnd)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // 멀티 뷰포트: ImGui 창을 메인 창 밖으로 끌어내면 진짜 OS 창(작업 표시줄에도 나옴)이 된다.
    // 셰이더 편집 창은 처음부터 독립 창으로 뜬다 (shader_file.cpp 의 DrawShaderEditor).
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(g_dpi);
    style.FontScaleDpi = g_dpi;
    style.FontSizeBase = 17.0f;
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;   // 독립 창은 뒤가 비치면 안 되니까 불투명하게
    LoadFonts(io);

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
}

// 창 메시지를 다 처리한다. 창이 닫혔으면 false
static bool PumpMessages()
{
    MSG msg;
    bool alive = true;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) alive = false;
    }
    return alive;
}

// 한 프레임: 되읽기 결과 꺼내기 -> UI -> 장면 GPU 명령 -> ImGui + Present
// minimized = 메인 창이 최소화됨. 그래도 독립 창(셰이더 창)은 계속 움직여야 하니
// UI 와 독립 창은 그리고, 보이지 않는 메인 창의 그리기(장면 + Present)만 건너뛴다.
static void RenderOneFrame(bool minimized)
{
    static const float clear[4] = { 0.06f, 0.06f, 0.08f, 1.0f };

    // 1) 이 프레임 자원 세트가 비었으면 장면이 되읽기 결과를 꺼낸다
    WaitCurrentFrameSlot();
    g_scene->OnFrameReused(g_dx.frameIndex);

    // 2) UI
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    static Canvas canvas{ 0, 0, 1, 1 };   // 최소화 중에는 마지막 크기를 그대로 쓴다
    if (!minimized)
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();   // 메인 창 (x, y 는 모니터 기준 위치)
        const float panelW = std::min(g_panelWidth * g_dpi, vp->Size.x * 0.6f);
        canvas = Canvas{ vp->Pos.x, vp->Pos.y, std::max(1.0f, vp->Size.x - panelW), std::max(1.0f, vp->Size.y) };
        DrawPanel(panelW);
        g_scene->OnCanvas(canvas);
    }
    else g_scene->OnMinimized(canvas);   // 셰이더 창만 (고치고 저장하면 컴파일도 됨)
    ImGui::Render();

    // 3) GPU 명령 기록 -> 4) ImGui 덮어 그리기 + Present  (메인 창이 보일 때만)
    if (!minimized)
    {
        ID3D12GraphicsCommandList* cl = BeginFrame(clear);
        g_scene->OnRender(cl, canvas);
        EndFrame();
    }

    // 5) 메인 창 밖으로 나간 ImGui 창(셰이더 편집 창 등)을 각자의 OS 창에 그린다
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
}

// 개발용 자동 테스트: 장면의 AutoTest 를 한 걸음 돌리고, 끝났으면 결과를 남기고 true
static bool AutoTestStep(int& testFrame, int& failures)
{
    if (!g_scene->AutoTest(testFrame++, failures) && testFrame <= 5000) return false;
    char line[128]; snprintf(line, sizeof line, "LAB_AUTOTEST %s failures=%d frames=%d\n", failures ? "FAIL" : "PASS", failures, testFrame);
    FILE* f = fopen("lab_autotest.log", "a"); if (f) { fputs(line, f); fclose(f); }
    OutputDebugStringA(line);
    return true;
}

// 메인 루프. 돌려주는 값 = 자동 테스트 실패 수
static int RunMainLoop(HWND hwnd)
{
    // LAB_AUTOTEST=1 이면 장면마다 AutoTest 를 돌리고 결과를 남긴 뒤 끝낸다
    const bool autotest = GetEnvironmentVariableA("LAB_AUTOTEST", nullptr, 0) > 0;
    int testFrame = 0, failures = 0;
    if (autotest) g_dx.vsync = false;

    while (PumpMessages())
    {
        const bool minimized = IsIconic(hwnd) != FALSE;
        if (minimized) Sleep(15);   // 메인 창 VSync 가 없으니 독립 창만 돌 때 CPU 를 덜 쓰게 (약 60번/초)
        else if (g_resizePending) { g_resizePending = false; ResizeSwapChain(g_newW, g_newH); }
        RenderOneFrame(minimized);
        if (autotest && AutoTestStep(testFrame, failures)) break;
    }
    return failures;
}

static void Shutdown()
{
    WaitIdle();
    if (g_scene) g_scene->Shutdown();
    g_scene.reset();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ShutdownD3D();
}

// DirectX-Graphics-Samples 의 Win32Application::Run 처럼: 창 -> 초기화 -> 루프 -> 정리
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    HWND hwnd = CreateMainWindow(hInst);
    InitD3D(hwnd);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    InitImGui(hwnd);
    SwitchScene(0);

    int failures = RunMainLoop(hwnd);
    Shutdown();
    return failures ? 2 : 0;
}
