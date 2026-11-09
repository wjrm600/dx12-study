// =====================================================================
//  core.cpp  -  DX12 뼈대 (발표용 메모리 데모의 초기화 부분을 단순하게 정리한 것)
//
//  데이터 흐름(발표와 같은 순서):
//    CPU가 명령 기록 -> 큐에 제출 -> GPU가 백 버퍼에 그림 -> 스왑체인 Present -> 모니터
// =====================================================================
#include "core.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <algorithm>
#include <cstdio>
#include <vector>

Dx g_dx;

void ShowMessage(const char* utf8)
{
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 2048);
    MessageBoxW(g_dx.hwnd, w, L"DX12 Study Lab", MB_ICONERROR);
}

void Fatal(const char* what, HRESULT hr, const char* file, int line)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "실패: %s\nHRESULT = 0x%08X\n%s:%d", what, (unsigned)hr, file, line);
    ShowMessage(buf);
    ExitProcess(1);
}

double NowMs()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------
// 디스크립터 자리 관리: "빈 자리 번호 목록"에서 꺼내 쓰고 돌려준다
// ---------------------------------------------------------------------
static std::vector<UINT> g_freeSrv, g_freeRtv;

void AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (g_freeSrv.empty()) Fatal("SRV 힙 자리가 모자람", E_OUTOFMEMORY, __FILE__, __LINE__);
    UINT i = g_freeSrv.back(); g_freeSrv.pop_back();
    cpu->ptr = g_dx.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)i * g_dx.srvInc;
    if (gpu) gpu->ptr = g_dx.srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)i * g_dx.srvInc;
}
void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    g_freeSrv.push_back((UINT)((cpu.ptr - g_dx.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_dx.srvInc));
}
D3D12_CPU_DESCRIPTOR_HANDLE AllocRtv()
{
    if (g_freeRtv.empty()) Fatal("RTV 힙 자리가 모자람", E_OUTOFMEMORY, __FILE__, __LINE__);
    UINT i = g_freeRtv.back(); g_freeRtv.pop_back();
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_dx.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g_dx.rtvInc;
    return h;
}
void FreeRtv(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    g_freeRtv.push_back((UINT)((cpu.ptr - g_dx.rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_dx.rtvInc));
}

// ---------------------------------------------------------------------
// 도우미
// ---------------------------------------------------------------------
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

ComPtr<ID3DBlob> CompileShader(const char* src, const char* name, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
    {
        std::string msg = std::string("셰이더 컴파일 실패: ") + name + " / " + entry + "\n";
        if (err) msg += (const char*)err->GetBufferPointer();
        ShowMessage(msg.c_str());
        ExitProcess(1);
    }
    return code;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC& desc)
{
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr))
    {
        ShowMessage(err ? (const char*)err->GetBufferPointer() : "루트 시그니처 직렬화 실패");
        ExitProcess(1);
    }
    ComPtr<ID3D12RootSignature> rs;
    HR(g_dx.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    return rs;
}

ComPtr<ID3D12Resource> CreateBuffer(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    HR(g_dx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

// ---------------------------------------------------------------------
// 펜스: "GPU야, 여기까지 끝나면 번호표 N을 걸어 줘" -> CPU는 번호표를 보고 기다린다
// ---------------------------------------------------------------------
static void WaitFence(uint64_t v)
{
    if (g_dx.fence->GetCompletedValue() < v)
    {
        HR(g_dx.fence->SetEventOnCompletion(v, g_dx.fenceEvent));
        WaitForSingleObject(g_dx.fenceEvent, INFINITE);
    }
}
void WaitIdle()
{
    HR(g_dx.queue->Signal(g_dx.fence.Get(), ++g_dx.fenceValue));
    WaitFence(g_dx.fenceValue);
}

// ---------------------------------------------------------------------
// 초기화
// ---------------------------------------------------------------------
static void CreateBackBufferViews()
{
    for (UINT i = 0; i < kBackBufferCount; i++)
    {
        HR(g_dx.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_dx.backBuffers[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_dx.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g_dx.rtvInc;   // RTV 0..2 번 자리는 백 버퍼 전용
        g_dx.device->CreateRenderTargetView(g_dx.backBuffers[i].Get(), nullptr, h);
    }
}

static void CreateFactory()
{
    UINT factoryFlags = 0;
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> dbg;   // 디버그 빌드: 잘못 쓰면 출력 창에 이유를 알려 주는 "검사관"
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); factoryFlags |= DXGI_CREATE_FACTORY_DEBUG; }
    }
#endif
    HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&g_dx.factory)));
}

// 1) 장치(Device): 고성능 GPU(외장 그래픽)를 먼저 고른다
static void CreateDevice()
{
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; g_dx.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter.Reset(); continue; }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dx.device))))
        {
            char name[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
            g_dx.adapterName = name;
            break;
        }
        adapter.Reset();
    }
    if (!g_dx.device) Fatal("D3D12CreateDevice (DX12를 지원하는 GPU가 없음)", E_FAIL, __FILE__, __LINE__);
}

// VSync 를 끌 때 화면 찢어짐(tearing)을 허용할 수 있는지
static void CheckTearingSupport()
{
    BOOL tearing = FALSE;
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(g_dx.factory.As(&f5)) && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
        g_dx.tearing = tearing != FALSE;
}

// 2) 명령 큐: CPU가 쓴 "할 일 목록"을 GPU에게 넘기는 창구. 펜스 = GPU 가 어디까지 했는지 보는 번호표
static void CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(g_dx.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_dx.queue)));
    HR(g_dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx.fence)));
    g_dx.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

// 3) 스왑체인: 화면에 번갈아 내보낼 백 버퍼 3장
static void CreateSwapChain(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    g_dx.width = std::max<UINT>(1, rc.right - rc.left); g_dx.height = std::max<UINT>(1, rc.bottom - rc.top);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_dx.width; sd.Height = g_dx.height; sd.Format = kBackBufferFormat; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kBackBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_dx.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    HR(g_dx.factory->CreateSwapChainForHwnd(g_dx.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
    HR(sc1.As(&g_dx.swapChain));
    g_dx.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
}

// 4) 디스크립터 힙: RTV(렌더 타깃 이름표), SRV(셰이더가 읽는 텍스처 이름표, ImGui와 공유)
static void CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kRtvHeapSize;
    HR(g_dx.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dx.rtvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSrvHeapSize; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(g_dx.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dx.srvHeap)));
    g_dx.rtvInc = g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dx.srvInc = g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = (int)kSrvHeapSize - 1; i >= 0; i--) g_freeSrv.push_back((UINT)i);
    for (int i = (int)kRtvHeapSize - 1; i >= (int)kBackBufferCount; i--) g_freeRtv.push_back((UINT)i);
}

// 5) 프레임 자원: 명령 할당자(명령을 적는 메모장)는 프레임마다 따로, 명령 리스트는 하나
static void CreateFrameResources()
{
    for (UINT i = 0; i < kFrameCount; i++)
        HR(g_dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_dx.alloc[i])));
    HR(g_dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_dx.alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_dx.list)));
    HR(g_dx.list->Close());
}

// DirectX-Graphics-Samples 의 LoadPipeline() 처럼 순서대로 한 단계씩
void InitD3D(HWND hwnd)
{
    g_dx.hwnd = hwnd;
    CreateFactory();          // 0) DXGI 팩토리 (+ 디버그 빌드면 디버그 레이어)
    CreateDevice();           // 1) 장치
    CheckTearingSupport();
    CreateCommandQueue();     // 2) 명령 큐 + 펜스
    CreateSwapChain(hwnd);    // 3) 스왑체인
    CreateDescriptorHeaps();  // 4) RTV / SRV 힙
    CreateBackBufferViews();
    CreateFrameResources();   // 5) 프레임마다 명령 할당자
}

void ShutdownD3D()
{
    WaitIdle();
    if (g_dx.fenceEvent) CloseHandle(g_dx.fenceEvent);
}

void ResizeSwapChain(UINT w, UINT h)
{
    WaitIdle();
    for (auto& b : g_dx.backBuffers) b.Reset();
    g_dx.width = std::max<UINT>(1, w); g_dx.height = std::max<UINT>(1, h);
    HR(g_dx.swapChain->ResizeBuffers(kBackBufferCount, g_dx.width, g_dx.height, kBackBufferFormat,
                                     g_dx.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    CreateBackBufferViews();
}

// ---------------------------------------------------------------------
// 한 프레임
// ---------------------------------------------------------------------
D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRtv()
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_dx.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)g_dx.swapChain->GetCurrentBackBufferIndex() * g_dx.rtvInc;
    return h;
}

ID3D12GraphicsCommandList* BeginFrame(const float clearColor[4])
{
    // 이 프레임 자원 세트로 그린 "예전 프레임"이 GPU에서 끝났는지 확인 (아직이면 기다림)
    WaitFence(g_dx.frameFence[g_dx.frameIndex]);
    ID3D12CommandAllocator* a = g_dx.alloc[g_dx.frameIndex].Get();
    HR(a->Reset());
    HR(g_dx.list->Reset(a, nullptr));
    ID3D12GraphicsCommandList* cl = g_dx.list.Get();

    ID3D12Resource* back = g_dx.backBuffers[g_dx.swapChain->GetCurrentBackBufferIndex()].Get();
    D3D12_RESOURCE_BARRIER b = Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferRtv();
    cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    return cl;
}

// 프레임 자원 세트 번호를 미리 알려 주는 함수 (BeginFrame 전에 장면이 되읽기 결과를 꺼낼 때 사용)
void WaitCurrentFrameSlot() { WaitFence(g_dx.frameFence[g_dx.frameIndex]); }

// ImGui 를 백 버퍼 위에 덮어 그리기
static void RecordImGui(ID3D12GraphicsCommandList* cl)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferRtv();
    D3D12_VIEWPORT vp{ 0, 0, (float)g_dx.width, (float)g_dx.height, 0, 1 };
    D3D12_RECT sc{ 0, 0, (LONG)g_dx.width, (LONG)g_dx.height };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_dx.srvHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
}

// 명령 리스트를 GPU 에 넘기고 화면에 내보낸 뒤, 이 프레임 세트에 번호표(펜스)를 건다
static void SubmitAndPresent(ID3D12GraphicsCommandList* cl)
{
    ID3D12CommandList* lists[] = { cl };
    g_dx.queue->ExecuteCommandLists(1, lists);
    HRESULT phr = g_dx.swapChain->Present(g_dx.vsync ? 1 : 0, (!g_dx.vsync && g_dx.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET)
    {
        char m[160]; snprintf(m, sizeof m, "GPU 장치가 제거됨 (reason 0x%08X)", (unsigned)g_dx.device->GetDeviceRemovedReason());
        ShowMessage(m); ExitProcess(1);
    }
    HR(g_dx.queue->Signal(g_dx.fence.Get(), ++g_dx.fenceValue));
    g_dx.frameFence[g_dx.frameIndex] = g_dx.fenceValue;   // "이 세트는 번호표 N이 걸리면 다시 써도 됨"
    g_dx.frameIndex = (g_dx.frameIndex + 1) % kFrameCount;
    g_dx.frameNo++;
}

void EndFrame()
{
    ID3D12GraphicsCommandList* cl = g_dx.list.Get();
    RecordImGui(cl);
    ID3D12Resource* back = g_dx.backBuffers[g_dx.swapChain->GetCurrentBackBufferIndex()].Get();
    D3D12_RESOURCE_BARRIER b = Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, &b);
    HR(cl->Close());
    SubmitAndPresent(cl);
}
