// =====================================================================
//  core.h  -  모든 장면이 같이 쓰는 DX12 뼈대
//
//  여기 있는 것: 장치(Device), 큐, 스왑체인, 백 버퍼, 펜스, 프레임 자원,
//                ImGui 준비, 셰이더 컴파일 같은 "매번 똑같은 준비물".
//  장면(scenes/*.cpp)은 이 준비물을 빌려 쓰고, 자기 주제만 코드로 쓴다.
//
//  비유: core 는 "주방"(가스레인지, 냄비, 싱크대)이고,
//        장면은 "오늘의 요리 레시피"다. 레시피마다 주방을 새로 짓지 않는다.
// =====================================================================
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

// 실패하면 어디서 실패했는지 메시지 상자로 보여 주고 끝낸다
void Fatal(const char* what, HRESULT hr, const char* file, int line);
void ShowMessage(const char* utf8);
#define HR(x) do { HRESULT hr__ = (x); if (FAILED(hr__)) Fatal(#x, hr__, __FILE__, __LINE__); } while (0)

double NowMs();

static const UINT        kFrameCount       = 2;   // 동시에 진행하는 프레임 수 (프레임 자원 세트 수)
static const UINT        kBackBufferCount  = 3;
static const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static const UINT        kSrvHeapSize      = 64;
static const UINT        kRtvHeapSize      = 16;  // 백 버퍼 3개 + 장면용 렌더 타깃 여유분

// 그림을 그릴 수 있는 영역 (오른쪽 패널을 뺀 나머지, 픽셀 단위)
struct Canvas { float x, y, w, h; };

struct Dx
{
    HWND hwnd = nullptr;
    ComPtr<IDXGIFactory6>             factory;
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<IDXGISwapChain3>           swapChain;
    ComPtr<ID3D12Resource>            backBuffers[kBackBufferCount];
    ComPtr<ID3D12DescriptorHeap>      rtvHeap, srvHeap;
    UINT                              rtvInc = 0, srvInc = 0;
    ComPtr<ID3D12CommandAllocator>    alloc[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    uint64_t                          fenceValue = 0;
    uint64_t                          frameFence[kFrameCount] = {};
    HANDLE                            fenceEvent = nullptr;
    UINT                              width = 1600, height = 900;
    UINT                              frameIndex = 0;   // 지금 쓰는 프레임 자원 세트 번호 (0 .. kFrameCount-1)
    uint64_t                          frameNo = 0;      // 지금까지 그린 프레임 수
    std::string                       adapterName;
    bool                              tearing = false;
    bool                              vsync = true;
};
extern Dx g_dx;

void InitD3D(HWND hwnd);
void ShutdownD3D();
void ResizeSwapChain(UINT w, UINT h);
void WaitIdle();

// 한 프레임의 시작과 끝 -------------------------------------------------
// BeginFrame: 이 프레임 자원 세트를 GPU가 다 썼는지 기다리고, 명령 목록을 열고,
//             백 버퍼를 "렌더 타깃" 상태로 바꾸고 지운다.
void WaitCurrentFrameSlot();   // 지금 프레임 자원 세트를 GPU가 다 쓸 때까지 기다림
ID3D12GraphicsCommandList* BeginFrame(const float clearColor[4]);
D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRtv();
// EndFrame: ImGui를 그리고, 백 버퍼를 "Present" 상태로 바꾸고, 제출하고, 화면에 낸다.
void EndFrame();

// 작은 도우미들 ----------------------------------------------------------
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
ComPtr<ID3DBlob> CompileShader(const char* src, const char* name, const char* entry, const char* target);
ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC& desc);
ComPtr<ID3D12Resource> CreateBuffer(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state);

// 디스크립터 자리 빌리기 (SRV 힙은 ImGui와 장면이 같이 쓴다)
void AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu);
D3D12_CPU_DESCRIPTOR_HANDLE AllocRtv();
void FreeRtv(D3D12_CPU_DESCRIPTOR_HANDLE cpu);
