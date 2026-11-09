// =====================================================================
//  DX12 메모리 최적화 데모  (발표 "DX12 메모리 구조 이해하기" 시연용)
//
//  왼쪽: 렌더링 화면 (가운데 무거운 행성 메시 + 주변을 도는 큐브 수천 개)
//  오른쪽: ImGui 패널. 시나리오마다 "최악 <-> 최적"을 바로 바꿔 보고
//          FPS / CPU·GPU 시간 / VRAM / 시스템 RAM 변화를 확인한다.
//
//  데이터 흐름(발표 순서와 같음):
//    CPU 원본(std::vector, 시스템 RAM) -> Upload Heap(시스템 RAM) -> Default Heap(VRAM)
//    -> GPU가 읽고 그림 -> 렌더 타깃 -> 스왑체인 Present -> 모니터
// =====================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <deque>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------
// Agility SDK: 최신 D3D12 런타임(D3D12Core.dll)을 exe 옆 D3D12 폴더에서 쓰게 한다.
// GPU Upload Heap(ReBAR) 기능은 최신 런타임이 있어야 켜지기 때문.
// ---------------------------------------------------------------------
#ifdef DEMO_AGILITY_SDK_VERSION
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = DEMO_AGILITY_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
#endif

// 오래된 Windows SDK 헤더에도 컴파일되도록 직접 정의한 값들
static const D3D12_HEAP_TYPE kHeapTypeGpuUpload = (D3D12_HEAP_TYPE)5;   // D3D12_HEAP_TYPE_GPU_UPLOAD
static const D3D12_FEATURE   kFeatureOptions16  = (D3D12_FEATURE)45;    // D3D12_FEATURE_D3D12_OPTIONS16
struct FeatureOptions16 { BOOL DynamicDepthBiasSupported; BOOL GPUUploadHeapSupported; };

// ---------------------------------------------------------------------
// 에러 처리
// ---------------------------------------------------------------------
static HWND g_hwnd = nullptr;
static void ShowMessage(const char* utf8, UINT icon = MB_ICONERROR)
{
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 2048);
    MessageBoxW(g_hwnd, w, L"DX12 Memory Demo", icon);
}
static void Fatal(const char* what, HRESULT hr, const char* file, int line)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "실패: %s\nHRESULT = 0x%08X\n%s:%d", what, (unsigned)hr, file, line);
    ShowMessage(buf);
    ExitProcess(1);
}
#define HR(x) do { HRESULT hr__ = (x); if (FAILED(hr__)) Fatal(#x, hr__, __FILE__, __LINE__); } while (0)

static double NowMs()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------
// 아주 작은 수학 라이브러리 (열 벡터 기준: clip = P * V * p)
// ---------------------------------------------------------------------
struct Vec3 { float x, y, z; };
static Vec3  operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3  Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static Vec3  Normalize(Vec3 a) { float l = sqrtf(Dot(a, a)); return { a.x / l, a.y / l, a.z / l }; }

struct Mat4 { float m[4][4]; };
static Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
static Mat4 PerspectiveLH(float fovY, float aspect, float zn, float zf)
{
    float ys = 1.0f / tanf(fovY * 0.5f), xs = ys / aspect;
    Mat4 r{};
    r.m[0][0] = xs; r.m[1][1] = ys;
    r.m[2][2] = zf / (zf - zn); r.m[2][3] = -zn * zf / (zf - zn);
    r.m[3][2] = 1.0f;
    return r;
}
static Mat4 LookAtLH(Vec3 eye, Vec3 at, Vec3 up)
{
    Vec3 z = Normalize(at - eye), x = Normalize(Cross(up, z)), y = Cross(z, x);
    Mat4 r{};
    r.m[0][0] = x.x; r.m[0][1] = x.y; r.m[0][2] = x.z; r.m[0][3] = -Dot(x, eye);
    r.m[1][0] = y.x; r.m[1][1] = y.y; r.m[1][2] = y.z; r.m[1][3] = -Dot(y, eye);
    r.m[2][0] = z.x; r.m[2][1] = z.y; r.m[2][2] = z.z; r.m[2][3] = -Dot(z, eye);
    r.m[3][3] = 1.0f;
    return r;
}

// ---------------------------------------------------------------------
// 데이터 형식
// ---------------------------------------------------------------------
struct Vertex   { float px, py, pz, nx, ny, nz, u, v; };       // 32바이트
struct Instance { float px, py, pz, scale, r, g, b, phase; };  // 32바이트 (큐브 1개)
struct FrameCB  { Mat4 viewProj; float time, meshScale, pad0, pad1; float lightDir[4]; float padding[40]; }; // 256바이트
static_assert(sizeof(FrameCB) == 256, "FrameCB must be 256 bytes");

static const UINT kMaxFrames      = 3;      // 프레임 자원 세트 수(최대 동시 진행 프레임)
static const UINT kMaxBackBuffers = 3;
static const int  kMaxObjects     = 20000;
static const int  kMaxPerObjectCB = 3000;   // "물체마다 새 버퍼" 모드의 상한
static const UINT kTsPerFrame     = 4;      // GPU 타임스탬프 개수
static const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT kDepthFormat      = DXGI_FORMAT_D32_FLOAT;
static const uint64_t kPressureChunk = 128ull << 20;  // VRAM 압박 실험: 128MB씩 할당
static const int kPressureTouchPerFrame = 8;          // 한 프레임에 사용하는 덩어리 최대 개수 (8 x 128MB = 1GB)
// 안전장치: 한 프레임에 너무 많은 메모리를 요구하면 GPU가 2초 넘게 멈춰(TDR) 드라이버가 리셋되거나
// 파란 화면(VIDEO_TDR_FAILURE)이 날 수 있다. 그래서 할당량과 프레임당 사용량에 상한을 둔다.

// ---------------------------------------------------------------------
// 힙 종류별 "우리 앱이 직접 만든" 메모리 추적
// ---------------------------------------------------------------------
enum HeapKind { HK_UPLOAD, HK_DEFAULT, HK_GPU_UPLOAD, HK_READBACK, HK_COUNT };
static const char* kHeapKindName[HK_COUNT] = { "Upload Heap (시스템 RAM)", "Default Heap (VRAM)", "GPU Upload Heap (VRAM, ReBAR)", "Readback Heap (시스템 RAM)" };
static int64_t g_heapBytes[HK_COUNT] = {};
static int     g_heapCount[HK_COUNT] = {};
static int     g_resourcesCreatedThisFrame = 0;

static ComPtr<ID3D12Device> g_device;

struct GpuRes
{
    ComPtr<ID3D12Resource> res;
    uint64_t size = 0, alloc = 0;
    int kind = 0;
    uint8_t* mapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS VA() const { return res->GetGPUVirtualAddress(); }
    ~GpuRes() { if (res) { g_heapBytes[kind] -= (int64_t)alloc; g_heapCount[kind]--; } }
};
using ResPtr = std::unique_ptr<GpuRes>;

static D3D12_RESOURCE_DESC BufferDesc(uint64_t size)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// 모든 리소스는 이 함수로 만든다 -> 힙 종류별 바이트 수가 자동으로 집계됨
// ===== [메모리 표시] 모든 GPU 리소스를 여기서 만들고 힙 종류별 MB를 센다 =====
static ResPtr CreateTracked(HeapKind kind, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                            const D3D12_CLEAR_VALUE* clear, bool map, HRESULT* outHr = nullptr)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = kind == HK_UPLOAD ? D3D12_HEAP_TYPE_UPLOAD
            : kind == HK_DEFAULT ? D3D12_HEAP_TYPE_DEFAULT
            : kind == HK_READBACK ? D3D12_HEAP_TYPE_READBACK : kHeapTypeGpuUpload;
    auto r = std::make_unique<GpuRes>();
    HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&r->res));
    if (outHr) *outHr = hr;
    if (FAILED(hr)) { r->res.Reset(); return nullptr; }
    r->kind = kind;
    r->size = desc.Width;
    // 실제로 잡히는 크기(버퍼는 최소 64KB 단위!)
    r->alloc = g_device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    g_heapBytes[kind] += (int64_t)r->alloc;
    g_heapCount[kind]++;
    g_resourcesCreatedThisFrame++;
    if (map)
    {
        D3D12_RANGE noRead{ 0, 0 };
        void* p = nullptr;
        HRESULT mhr = r->res->Map(0, kind == HK_READBACK ? nullptr : &noRead, &p);
        if (FAILED(mhr)) { if (outHr) *outHr = mhr; return nullptr; }
        r->mapped = (uint8_t*)p;
    }
    return r;
}

static ResPtr CreateBuffer(HeapKind kind, uint64_t size, bool map, HRESULT* outHr = nullptr)
{
    // Upload 힙은 GENERIC_READ, Readback 힙은 COPY_DEST로 시작해야 한다는 규칙이 있다.
    D3D12_RESOURCE_STATES st = kind == HK_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                             : kind == HK_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                             : D3D12_RESOURCE_STATE_COMMON;
    return CreateTracked(kind, BufferDesc(size), st, nullptr, map, outHr);
}

static ResPtr MustBuffer(HeapKind kind, uint64_t size, bool map)
{
    HRESULT hr;
    ResPtr r = CreateBuffer(kind, size, map, &hr);
    if (!r) Fatal("CreateBuffer", hr, __FILE__, __LINE__);
    return r;
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}

// ---------------------------------------------------------------------
// 셰이더 (실행 중에 D3DCompile로 컴파일 -> 별도 빌드 도구가 필요 없음)
// ---------------------------------------------------------------------
#ifdef DEMO_TEST_WINE_SHADER   // 테스트 전용: Wine의 HLSL 컴파일러는 StructuredBuffer를 못 읽음
#define INST_DECL "struct Inst { float4 posScale; float4 color; };\n"
#define IID_DECL ""
#define INST_READ "float fi = (float)(gBase); d.posScale = float4(cos(fi*2.4)*5, sin(fi*0.7)*3, sin(fi*2.4)*5, 0.16); d.color = float4(frac(fi*0.37), frac(fi*0.11), frac(fi*0.23), frac(fi*0.618));"
#else
#define INST_DECL "struct Inst { float4 posScale; float4 color; };\nStructuredBuffer<Inst> gInst : register(t0);   // 큐브 위치/색 (루트 SRV)\n"
#define IID_DECL "uint iid : SV_InstanceID;"
#define INST_READ "d = gInst[gBase + i.iid];"
#endif
static const char* kShaderSrc = R"HLSL(
cbuffer RootConsts : register(b0) { uint gBase; uint gMode; uint gPad0; uint gPad1; };  // 루트 상수
cbuffer Frame      : register(b1) { row_major float4x4 gViewProj; float4 gTime; float4 gLightDir; };
cbuffer Obj        : register(b2) { float4 oPosScale; float4 oColor; };                 // "물체마다 버퍼" 모드
)HLSL" INST_DECL R"HLSL(
Texture2D    gTex  : register(t1);             // 체커 텍스처 (디스크립터 테이블)
SamplerState gSamp : register(s0);             // 정적 샘플러

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; )HLSL" IID_DECL R"HLSL( };
struct VSOut { float4 pos : SV_Position; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float3 col : COLOR0; };

float3x3 RotY(float a) { float s = sin(a), c = cos(a); return float3x3(c, 0, s, 0, 1, 0, -s, 0, c); }
float3x3 RotX(float a) { float s = sin(a), c = cos(a); return float3x3(1, 0, 0, 0, c, -s, 0, s, c); }

VSOut VSMain(VSIn i)
{
    VSOut o;
    float3 p, n;
    if (gMode == 2)            // 가운데 무거운 행성 메시
    {
        float3x3 R = RotY(gTime.x * 0.15);
        p = mul(R, i.pos) * gTime.y;
        n = mul(R, i.nrm);
        o.col = float3(0.55, 0.75, 1.0);
    }
    else
    {
        float4 ps, c;
        if (gMode == 1) { ps = oPosScale; c = oColor; }
        else { Inst d; )HLSL" INST_READ R"HLSL( ps = d.posScale; c = d.color; }
        float a = gTime.x * 1.5 + c.w * 6.2831;
        float3x3 R = mul(RotY(a), RotX(a * 0.7));
        p = mul(R, i.pos) * ps.w + ps.xyz;
        n = mul(R, i.nrm);
        o.col = c.rgb;
    }
    o.pos = mul(gViewProj, float4(p, 1));
    o.nrm = n;
    o.uv = i.uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 n = normalize(i.nrm);
    float l = saturate(dot(n, -gLightDir.xyz)) * 0.8 + 0.2;
    float3 t = gTex.Sample(gSamp, i.uv).rgb;
    return float4(i.col * t * l, 1);
}
)HLSL";

// ---------------------------------------------------------------------
// 설정 (패널에서 바꾸는 값들)
// ---------------------------------------------------------------------
enum MeshHeap   { MH_UPLOAD = 0, MH_DEFAULT = 1, MH_GPU_UPLOAD = 2 };
enum BufferMode { BM_REUSE = 0, BM_NEW_BIG = 1, BM_NEW_PER_OBJECT = 2 };
enum Readback   { RB_OFF = 0, RB_WAIT_NOW = 1, RB_DELAYED = 2 };

struct Settings
{
    // 1. 정점 데이터 위치
    int  meshTrisK = 600;            // 행성 메시 삼각형 수(천 개)
    int  meshRepeat = 2;             // 같은 자리에 몇 번 그릴지 (정점 읽기량 증폭)
    int  meshHeap = MH_DEFAULT;
    bool meshReupload = false;       // 매 프레임 CPU -> GPU로 다시 보내기
    // 2. 리소스 생성
    int  bufferMode = BM_REUSE;
    // 3. 배리어
    int  uselessBarriers = 0;
    // 4. 동기화
    int  framesInFlight = 2;
    int  cpuLogicMs = 0;             // 게임 로직 흉내: CPU가 매 프레임 이만큼 일함 (프리셋에 포함 안 됨)
    // 5. 드로우콜
    int  objectCount = 5000;
    bool instancing = true;
    // 6. 상태 변경
    bool resetStatePerDraw = false;
    bool recompilePso = false;
    // 7. Readback
    int  readback = RB_OFF;
    // 8. 스왑체인
    int  backBuffers = 3;
    bool vsync = false;
    // 9. VRAM 압박
    int  pressureMB = 0;
    int  pressureHeap = 0;           // 0 = Default(VRAM), 1 = Upload(시스템 RAM)
    bool pressureTouch = false;
    bool pressureEvict = false;
    // 10. 텍스처
    int  texSize = 2;                // 0 = 256, 1 = 1024(1K), 2 = 4096(4K)
    bool texMips = true;             // 밉맵(미리 줄여 둔 작은 그림들) 만들기
    bool texMipTint = false;         // 밉 단계마다 색 칠하기 (눈으로 확인용, 프리셋에 포함 안 됨)
    bool texRecreate = false;        // 매 프레임 텍스처를 새로 만들고 다시 올리기
};

static bool SameSettings(const Settings& a, const Settings& b)
{
    return a.meshTrisK == b.meshTrisK && a.meshRepeat == b.meshRepeat && a.meshHeap == b.meshHeap && a.meshReupload == b.meshReupload
        && a.bufferMode == b.bufferMode && a.uselessBarriers == b.uselessBarriers && a.framesInFlight == b.framesInFlight
        && a.objectCount == b.objectCount && a.instancing == b.instancing && a.resetStatePerDraw == b.resetStatePerDraw
        && a.recompilePso == b.recompilePso && a.readback == b.readback && a.backBuffers == b.backBuffers && a.vsync == b.vsync
        && a.cpuLogicMs == b.cpuLogicMs && a.pressureMB == b.pressureMB && a.pressureHeap == b.pressureHeap && a.pressureTouch == b.pressureTouch && a.pressureEvict == b.pressureEvict
        && a.texSize == b.texSize && a.texMips == b.texMips && a.texMipTint == b.texMipTint && a.texRecreate == b.texRecreate;
}

static void PresetBest(Settings& s)
{
    s.meshHeap = MH_DEFAULT; s.meshReupload = false; s.bufferMode = BM_REUSE; s.uselessBarriers = 0;
    s.framesInFlight = 2; s.instancing = true; s.resetStatePerDraw = false; s.recompilePso = false;
    s.readback = RB_OFF; s.backBuffers = 3; s.vsync = false;
    s.texMips = true; s.texRecreate = false;
}
static Settings g_s;          // 패널에서 바꾸는 지금 설정 (프레임 함수들에서는 s 라는 이름으로 씀)
static void PresetWorst(Settings& s)
{
    s.meshHeap = MH_UPLOAD; s.meshReupload = true; s.bufferMode = BM_NEW_PER_OBJECT; s.uselessBarriers = 300;
    s.framesInFlight = 1; s.instancing = false; s.resetStatePerDraw = true; s.recompilePso = true;
    s.readback = RB_WAIT_NOW; s.backBuffers = 2; s.vsync = false;
    s.texMips = false; s.texRecreate = true;
}

// ---------------------------------------------------------------------
// D3D12 전역 객체
// ---------------------------------------------------------------------
struct FrameRes
{
    ComPtr<ID3D12CommandAllocator> alloc;
    ResPtr frameCB;          // Upload: 프레임 상수(카메라 등)
    ResPtr inst;             // Upload: 큐브 위치 데이터 (재사용 모드)
    ResPtr tsReadback;       // Readback: GPU 타임스탬프 결과
    ResPtr statsReadback;    // Readback: 파이프라인 통계(정점 수 등)
    bool statsValid = false;
    ResPtr sceneReadback;    // Readback: 화면 되읽기
    ResPtr meshFrame;        // 매 프레임 재전송 모드에서 쓰는 이 프레임 전용 메시 버퍼
    D3D12_GPU_DESCRIPTOR_HANDLE texBind{};   // 이번 프레임 셰이더 t1에 꽂을 텍스처 (보통 g_texGpu)
    std::vector<ResPtr> trash;                         // GPU가 다 쓴 뒤 지울 리소스
    std::vector<ComPtr<ID3D12PipelineState>> trashPso;
    bool tsValid = false, rbValid = false;
    uint64_t frameNo = 0;
};

static ComPtr<IDXGIFactory6>            g_factory;
static ComPtr<IDXGIAdapter3>            g_adapter;
static ComPtr<ID3D12CommandQueue>       g_directQueue, g_copyQueue;
static ComPtr<ID3D12GraphicsCommandList> g_list, g_copyList;
static ComPtr<ID3D12CommandAllocator>   g_copyAlloc;
static ComPtr<ID3D12Fence>              g_fence, g_copyFence;
static uint64_t                         g_fenceValue = 0, g_copyFenceValue = 0;
static HANDLE                           g_fenceEvent = nullptr;
static ComPtr<IDXGISwapChain3>          g_swapChain;
static ComPtr<ID3D12Resource>           g_backBuffers[kMaxBackBuffers];
static UINT                             g_bbCount = 3;
static bool                             g_tearingSupported = false;
static ComPtr<ID3D12DescriptorHeap>     g_rtvHeap, g_dsvHeap, g_srvHeap;
static UINT                             g_rtvInc = 0, g_srvInc = 0;
static ResPtr                           g_depth;
static ComPtr<ID3D12RootSignature>      g_rootSig;
static ComPtr<ID3DBlob>                 g_vsBlob, g_psBlob;
static ComPtr<ID3D12PipelineState>      g_psoA, g_psoB, g_psoWireA, g_psoWireB;   // 면 / 와이어프레임
static bool g_wireframe = false;
static ComPtr<ID3D12QueryHeap>          g_queryHeap;
static ComPtr<ID3D12QueryHeap>          g_pipeStatsHeap;   // GPU가 센 정점/삼각형 수 (없으면 생략)
static uint64_t                         g_tsFreq = 1;
static FrameRes                         g_frames[kMaxFrames];
static uint64_t                         g_frameNo = 0;
static uint64_t                         g_frameFence[8] = {};
static UINT                             g_width = 1600, g_height = 900;
static bool                             g_resizePending = false;
static UINT                             g_newW = 0, g_newH = 0;

// 기능 지원
static bool g_gpuUploadSupported = false, g_uma = false, g_cacheCoherentUma = false;
static std::string g_adapterName;
static uint64_t g_dedicatedVram = 0, g_sharedSys = 0;

// 씬 리소스
static ResPtr g_cubeVB, g_cubeIB, g_tex;
static D3D12_GPU_DESCRIPTOR_HANDLE g_texGpu{};
static std::vector<Vertex> g_meshCpu;        // CPU 원본 (시스템 RAM, std::vector)
static int    g_meshGenTrisK = -1;
static ResPtr g_meshStatic;                  // 한 번 올려 둔 메시 버퍼
static int    g_meshBuiltHeap = -1, g_meshBuiltTrisK = -1; static bool g_meshBuiltReupload = false;
static double g_meshUploadMs = 0.0;          // 복사 큐 업로드 시간
static std::string g_meshError;

// VRAM 압박 실험
static std::vector<ResPtr> g_pressure;
static int  g_pressureBuiltHeap = 0;
static bool g_pressureEvicted = false;
static ResPtr g_pressureSink;
static std::string g_pressureError;

// 화면 되읽기 결과
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_rbFootprint{};
static uint64_t g_rbBytes = 0;
static float    g_rbColor[3] = {};
static int      g_rbAgeFrames = 0;

// 측정값
struct Stats
{
    double frameMs = 0, waitMs = 0, cpuWorkMs = 0, presentMs = 0;
    double gpuTotalMs = 0, gpuMeshMs = 0, gpuCubesMs = 0;
    int drawCalls = 0, barriers = 0, psoSets = 0, rootSigSets = 0, created = 0;
    uint64_t meshBytesReadPerFrame = 0, meshBytesCopiedPerFrame = 0;
    uint64_t texBytesThisFrame = 0;                 // 10번: 이번 프레임에 새로 만든 텍스처 크기 (VRAM 쪽)
    // 정점 수 (CPU가 계산)
    uint64_t uniqueVerts = 0, uniqueTris = 0;       // 화면에 있는 물체들의 정점/삼각형 (데이터 기준, 1번씩)
    uint64_t submittedVerts = 0, submittedTris = 0; // 이번 프레임 GPU에 그리라고 보낸 양 (반복, 인덱스 포함)
    // 정점 수 (GPU가 직접 센 값: 파이프라인 통계 쿼리, 3프레임 전 결과)
    bool gpuStatsValid = false;
    uint64_t gpuIAVerts = 0, gpuVSInvocations = 0, gpuIAPrims = 0, gpuRasterPrims = 0, gpuPSInvocations = 0;
};
static Stats g_stats;

// N초 평균 (숫자가 너무 빨리 바뀌어서 읽기 힘들 때)
struct AvgSample { double t, frame, gpu, wait, work, present; };
static std::deque<AvgSample> g_avgSamples;
static float  g_avgSeconds = 5.0f;
static double g_avgStart = 0.0;                  // 평균을 다시 모으기 시작한 시각
struct AvgStats { double fps = 0, frame = 0, gpu = 0, wait = 0, work = 0, present = 0, covered = 0; };
static AvgStats g_avg;
static void ResetAverage(double now) { g_avgSamples.clear(); g_avgStart = now; g_avg = AvgStats{}; }
static void UpdateAverage(double now)
{
    g_avgSamples.push_back({ now, g_stats.frameMs, g_stats.gpuTotalMs, g_stats.waitMs, g_stats.cpuWorkMs, g_stats.presentMs });
    while (!g_avgSamples.empty() && g_avgSamples.front().t < now - g_avgSeconds * 1000.0) g_avgSamples.pop_front();
    AvgStats a; double n = (double)g_avgSamples.size();
    for (const AvgSample& x : g_avgSamples) { a.frame += x.frame; a.gpu += x.gpu; a.wait += x.wait; a.work += x.work; a.present += x.present; }
    if (n > 0)
    {
        a.fps = a.frame > 0 ? n * 1000.0 / a.frame : 0.0;   // 전체 프레임 수 / 전체 시간
        a.frame /= n; a.gpu /= n; a.wait /= n; a.work /= n; a.present /= n;
    }
    a.covered = std::min((double)g_avgSeconds, (now - g_avgStart) / 1000.0);
    g_avg = a;
}
static const int kHist = 240;
static float g_histFrame[kHist] = {}, g_histGpu[kHist] = {}, g_histVram[kHist] = {}, g_histRam[kHist] = {};
static int   g_histPos = 0;

// 메모리 정보
struct MemInfo
{
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
    uint64_t sysTotal = 0, sysAvail = 0, workingSet = 0, privateBytes = 0;
};
static MemInfo g_mem;

// 디스크립터 할당기 (ImGui와 같이 씀)
struct SrvAllocator
{
    std::vector<int> freeList;
    void Init(UINT n) { for (int i = (int)n - 1; i >= 0; i--) freeList.push_back(i); }
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g)
    {
        int idx = freeList.back(); freeList.pop_back();
        c->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)idx * g_srvInc;
        g->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)idx * g_srvInc;
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        freeList.push_back((int)((c.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srvInc));
    }
};
static SrvAllocator g_srvAlloc;
static bool g_panelHidden = false;

// ---------------------------------------------------------------------
// 펜스 / 동기화
// ---------------------------------------------------------------------
static void WaitFence(ID3D12Fence* f, uint64_t v)
{
    if (f->GetCompletedValue() < v)
    {
        HR(f->SetEventOnCompletion(v, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}
static void WaitIdle()
{
    HR(g_directQueue->Signal(g_fence.Get(), ++g_fenceValue));
    WaitFence(g_fence.Get(), g_fenceValue);
}

// 복사 큐로 한 번 실행하고 끝날 때까지 기다린다 (Copy Queue + Fence)
template <class F> static double RunOnCopyQueue(F record)
{
    double t0 = NowMs();
    HR(g_copyAlloc->Reset());
    HR(g_copyList->Reset(g_copyAlloc.Get(), nullptr));
    record(g_copyList.Get());
    HR(g_copyList->Close());
    ID3D12CommandList* lists[] = { g_copyList.Get() };
    g_copyQueue->ExecuteCommandLists(1, lists);
    HR(g_copyQueue->Signal(g_copyFence.Get(), ++g_copyFenceValue));
    WaitFence(g_copyFence.Get(), g_copyFenceValue);
    return NowMs() - t0;
}

// ---------------------------------------------------------------------
// 셰이더 / PSO
// ---------------------------------------------------------------------
static bool CompileShaders(ComPtr<ID3DBlob>& vs, ComPtr<ID3DBlob>& ps)
{
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3DCompile(kShaderSrc, strlen(kShaderSrc), "demo.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vs, &err)))
    { ShowMessage(err ? (const char*)err->GetBufferPointer() : "VS compile failed"); return false; }
    if (FAILED(D3DCompile(kShaderSrc, strlen(kShaderSrc), "demo.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &err)))
    { ShowMessage(err ? (const char*)err->GetBufferPointer() : "PS compile failed"); return false; }
    return true;
}

static ComPtr<ID3D12PipelineState> CreatePso(ID3DBlob* vs, ID3DBlob* ps, int depthBias, bool wire = false, bool blend = false)
{
    static const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = g_rootSig.Get();
    d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    d.InputLayout = { layout, 3 };
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.RasterizerState.FillMode = wire ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;   // 와이어프레임 = 삼각형 테두리만
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthBias = depthBias;           // PSO B는 이 값만 다르다 (화면은 같음)
    d.RasterizerState.DepthClipEnable = TRUE;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (blend)   // PSO B: 알파 블렌딩 켬 (알파가 1이라 화면은 같지만 GPU는 블렌드 장치를 다시 설정해야 함)
    {
        auto& rt = d.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA; rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    d.SampleMask = UINT_MAX;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = kBackBufferFormat;
    d.DSVFormat = kDepthFormat;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    HR(g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
    return pso;
}

static void CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1; range.BaseShaderRegister = 1;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER p[5]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;              // b0: 루트 상수 4개
    p[0].Constants = { 0, 0, 4 }; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;                          // b1: 프레임 상수
    p[1].Descriptor = { 1, 0 };   p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;                          // t0: 큐브 데이터
    p[2].Descriptor = { 0, 0 };   p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;                          // b2: 물체별 상수
    p[3].Descriptor = { 2, 0 };   p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;             // t1: 텍스처 (디스크립터 힙)
    p[4].DescriptorTable = { 1, &range }; p[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MaxLOD = D3D12_FLOAT32_MAX; s.ShaderRegister = 0; s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 5; rd.pParameters = p; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &s;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, err;
    HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    HR(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig)));
}

// ---------------------------------------------------------------------
// 메시 생성
// ---------------------------------------------------------------------
static void CreateCube()
{
    std::vector<Vertex> v; std::vector<uint16_t> idx;
    const float N[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    const float U[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{1,0,0},{-1,0,0},{1,0,0} };
    for (int f = 0; f < 6; f++)
    {
        float n[3] = { N[f][0], N[f][1], N[f][2] }, u[3] = { U[f][0], U[f][1], U[f][2] };
        float b[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
        uint16_t base = (uint16_t)v.size();
        const float su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
        for (int c = 0; c < 4; c++)
        {
            Vertex x;
            x.px = 0.5f * (n[0] + su[c] * u[0] + sv[c] * b[0]);
            x.py = 0.5f * (n[1] + su[c] * u[1] + sv[c] * b[1]);
            x.pz = 0.5f * (n[2] + su[c] * u[2] + sv[c] * b[2]);
            x.nx = n[0]; x.ny = n[1]; x.nz = n[2];
            x.u = su[c] * 0.5f + 0.5f; x.v = sv[c] * 0.5f + 0.5f;
            v.push_back(x);
        }
        uint16_t q[6] = { 0, 1, 2, 0, 2, 3 };
        for (uint16_t k : q) idx.push_back(base + k);
    }
    // 정적 데이터: Upload(임시) -> 복사 큐 -> Default(VRAM). 발표의 "정석" 경로.
    uint64_t vbSize = v.size() * sizeof(Vertex), ibSize = idx.size() * 2;
    g_cubeVB = MustBuffer(HK_DEFAULT, vbSize, false);
    g_cubeIB = MustBuffer(HK_DEFAULT, ibSize, false);
    ResPtr staging = MustBuffer(HK_UPLOAD, vbSize + ibSize, true);
    memcpy(staging->mapped, v.data(), vbSize);
    memcpy(staging->mapped + vbSize, idx.data(), ibSize);
    RunOnCopyQueue([&](ID3D12GraphicsCommandList* cl) {
        cl->CopyBufferRegion(g_cubeVB->res.Get(), 0, staging->res.Get(), 0, vbSize);
        cl->CopyBufferRegion(g_cubeIB->res.Get(), 0, staging->res.Get(), vbSize, ibSize);
    });
}

// =====================================================================
// 10번 시나리오: 텍스처 (크기 256 / 1K / 4K, 밉맵, 매 프레임 새로 만들기)
// 텍스처는 파일 없이 코드로 만든다: 큰 체크무늬 + 16픽셀 격자 + 픽셀 단위 잡티(고해상도 느낌)
// =====================================================================
static const UINT kTexSizes[3] = { 256, 1024, 4096 };
static std::vector<std::vector<uint32_t>> g_texCpu;     // CPU 원본: 밉 단계별 픽셀 (0 = 원본 크기)
static int    g_texBuiltSize = -1, g_texBuiltMips = -1, g_texBuiltTint = -1;
static UINT   g_texMipCount = 1;
static double g_texGenMs = 0.0, g_texUploadMs = 0.0;     // 픽셀 계산 시간, 복사 큐 업로드 시간
static uint64_t g_texAllocBytes = 0;                     // 텍스처 1장이 VRAM에서 실제로 차지하는 크기
static D3D12_CPU_DESCRIPTOR_HANDLE g_texSrvCpu{};        // 평소에 쓰는 텍스처의 SRV 자리
static D3D12_CPU_DESCRIPTOR_HANDLE g_texFrameSrvCpu[kMaxFrames]{};   // "매 프레임 새로 만들기"용: 프레임 세트마다 SRV 자리 1개
static D3D12_GPU_DESCRIPTOR_HANDLE g_texFrameSrvGpu[kMaxFrames]{};

static D3D12_RESOURCE_DESC TextureDesc()
{
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = kTexSizes[g_texBuiltSize];
    td.DepthOrArraySize = 1; td.MipLevels = (UINT16)g_texMipCount;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    return td;
}

// CPU에서 픽셀 만들기 (원본 + 밉맵). 밉맵 = 가로세로 절반씩 줄인 그림을 1x1까지 미리 만들어 둔 것
// 원본(0단계) 체크무늬 그림
static std::vector<uint32_t> MakeCheckerPixels(UINT size)
{
    std::vector<uint32_t> top((size_t)size * size);
    UINT block = size / 8;                                  // 큰 체크무늬 8x8칸 (크기와 상관없이 같은 모양)
    for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++)
        {
            bool c = ((x / block) ^ (y / block)) & 1;
            int g = c ? 245 : 190;
            if (x % 16 == 0 || y % 16 == 0) g -= 70;         // 16픽셀 격자: 해상도가 클수록 선이 촘촘해진다
            uint32_t h = (x * 73856093u) ^ (y * 19349663u); h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            g += (int)(h & 31) - 16;                           // 픽셀마다 잡티 -> 밉맵이 없으면 멀리서 반짝거림
            g = std::max(0, std::min(255, g));
            top[(size_t)y * size + x] = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
    return top;
}

// 2x2 픽셀 평균 -> 절반 크기 그림 하나 (밉 한 단계)
static std::vector<uint32_t> Downsample2x2(const std::vector<uint32_t>& src, UINT w)
{
    UINT sw = w * 2;
    std::vector<uint32_t> dst((size_t)w * w);
    for (UINT y = 0; y < w; y++)
        for (UINT x = 0; x < w; x++)
        {
            uint32_t q[4] = { src[(size_t)(2 * y) * sw + 2 * x], src[(size_t)(2 * y) * sw + 2 * x + 1],
                              src[(size_t)(2 * y + 1) * sw + 2 * x], src[(size_t)(2 * y + 1) * sw + 2 * x + 1] };
            uint32_t o = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++)
            {
                uint32_t sum = 0;
                for (uint32_t v : q) sum += (v >> (ch * 8)) & 255;
                o |= ((sum + 2) / 4) << (ch * 8);
            }
            dst[(size_t)y * w + x] = o;
        }
    return dst;
}

// 밉 단계마다 색을 입힌다: 0 = 원래 색, 1 = 초록, 2 = 파랑, 3 = 노랑, 4 = 빨강 ...
static void TintMipLevels()
{
    static const float k[7][3] = { {0.45f,1,0.45f}, {0.45f,0.6f,1}, {1,1,0.35f}, {1,0.45f,0.45f}, {0.4f,1,1}, {1,0.45f,1}, {0.7f,0.7f,0.7f} };
    for (size_t m = 1; m < g_texCpu.size(); m++)
        for (uint32_t& v : g_texCpu[m])
        {
            const float* t = k[(m - 1) % 7];
            uint32_t r = (uint32_t)((v & 255) * t[0]), g = (uint32_t)(((v >> 8) & 255) * t[1]), b = (uint32_t)(((v >> 16) & 255) * t[2]);
            v = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
}

// ===== [시나리오 10] 밉맵 만들기: 원본 -> 절반 -> ... -> 1x1 =====
static void GenerateTexturePixels(UINT size, bool mips, bool tint)
{
    g_texCpu.clear();
    g_texCpu.push_back(MakeCheckerPixels(size));
    if (mips)
    {
        for (UINT w = size / 2; w >= 1; w /= 2)
        {
            std::vector<uint32_t> dst = Downsample2x2(g_texCpu.back(), w);
            g_texCpu.push_back(std::move(dst));
            if (w == 1) break;
        }
        if (tint) TintMipLevels();
    }
    g_texMipCount = (UINT)g_texCpu.size();
}

// Upload 힙 임시 상자에 모든 밉 단계를 한 줄씩 복사하고, 복사 명령을 기록한다
// (텍스처는 줄 길이가 256바이트 배수여야 해서 GetCopyableFootprints가 알려 주는 RowPitch를 따른다)
static ResPtr StageAndRecordTextureCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, const D3D12_RESOURCE_DESC& td)
{
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(g_texMipCount);
    std::vector<UINT> rows(g_texMipCount); std::vector<UINT64> rowBytes(g_texMipCount); UINT64 total = 0;
    g_device->GetCopyableFootprints(&td, 0, g_texMipCount, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    ResPtr staging = CreateBuffer(HK_UPLOAD, total, true);
    if (!staging) return nullptr;
    for (UINT m = 0; m < g_texMipCount; m++)
    {
        UINT w = fp[m].Footprint.Width;
        for (UINT y = 0; y < rows[m]; y++)
            memcpy(staging->mapped + fp[m].Offset + (uint64_t)y * fp[m].Footprint.RowPitch, &g_texCpu[m][(size_t)y * w], (size_t)w * 4);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = m;
        src.pResource = staging->res.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[m];
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    return staging;
}

static void WriteTextureSrv(ID3D12Resource* tex, D3D12_CPU_DESCRIPTOR_HANDLE where)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = g_texMipCount;
    g_device->CreateShaderResourceView(tex, &sd, where);
}

// ===== [시나리오 10] 최적: 텍스처를 한 번 만들어 복사 큐로 VRAM에 올려 두기 =====
static void RebuildTexture(const Settings& s)
{
    WaitIdle();                                         // 지금 텍스처를 GPU가 쓰고 있을 수 있으니 다 끝나길 기다림
    if (g_texSrvCpu.ptr == 0)                           // 처음 한 번: SRV 자리 확보
    {
        g_srvAlloc.Alloc(&g_texSrvCpu, &g_texGpu);
        for (UINT i = 0; i < kMaxFrames; i++) g_srvAlloc.Alloc(&g_texFrameSrvCpu[i], &g_texFrameSrvGpu[i]);
    }
    g_tex.reset();
    g_texBuiltSize = s.texSize; g_texBuiltMips = s.texMips; g_texBuiltTint = s.texMipTint;
    double t0 = NowMs();
    GenerateTexturePixels(kTexSizes[s.texSize], s.texMips, s.texMipTint);
    g_texGenMs = NowMs() - t0;

    D3D12_RESOURCE_DESC td = TextureDesc();
    g_tex = CreateTracked(HK_DEFAULT, td, D3D12_RESOURCE_STATE_COMMON, nullptr, false);    // 1) VRAM에 자리
    if (!g_tex) Fatal("Create texture", E_FAIL, __FILE__, __LINE__);
    g_texAllocBytes = g_tex->alloc;
    ResPtr staging;
    g_texUploadMs = RunOnCopyQueue([&](ID3D12GraphicsCommandList* cl) {               // 2~4) 임시 상자 -> 복사 큐 -> 펜스
        staging = StageAndRecordTextureCopy(cl, g_tex->res.Get(), td);
    });
    if (!staging) Fatal("Texture staging buffer", E_OUTOFMEMORY, __FILE__, __LINE__);
    staging.reset();                                                                    // 복사가 끝났으니 임시 상자는 버림
    WriteTextureSrv(g_tex->res.Get(), g_texSrvCpu);                                    // 5) 명함(SRV)
}

// 행성 메시: 인덱스 없이 삼각형마다 정점 3개 -> GPU가 읽는 바이트 수가 정직하게 늘어난다
static void GenerateSphere(int trisK)
{
    g_meshCpu.clear();
    g_meshCpu.shrink_to_fit();
    g_meshGenTrisK = trisK;
    if (trisK <= 0) return;
    int seg = std::max(8, (int)sqrtf((float)trisK * 1000.0f));
    int rings = std::max(4, seg / 2);
    seg = std::max(8, (trisK * 1000) / (2 * rings));
    g_meshCpu.reserve((size_t)seg * rings * 6);
    auto P = [&](int i, int j) {
        float th = 3.14159265f * (float)i / rings, ph = 6.2831853f * (float)j / seg;
        float r = 1.0f + 0.035f * sinf(th * 9.0f) * sinf(ph * 7.0f) + 0.02f * sinf(ph * 23.0f + th * 5.0f);
        Vertex v;
        v.nx = sinf(th) * cosf(ph); v.ny = cosf(th); v.nz = sinf(th) * sinf(ph);
        v.px = v.nx * r; v.py = v.ny * r; v.pz = v.nz * r;
        v.u = 8.0f * (float)j / seg; v.v = 4.0f * (float)i / rings;
        return v;
    };
    for (int i = 0; i < rings; i++)
        for (int j = 0; j < seg; j++)
        {
            Vertex a = P(i, j), b = P(i + 1, j), c = P(i + 1, j + 1), d = P(i, j + 1);
            g_meshCpu.push_back(a); g_meshCpu.push_back(b); g_meshCpu.push_back(c);
            g_meshCpu.push_back(a); g_meshCpu.push_back(c); g_meshCpu.push_back(d);
        }
}

// ===== [시나리오 1] 정점 버퍼를 어느 힙에 만들지 정하는 곳 (Upload / Default / GPU Upload) =====
// Default 힙(VRAM): CPU가 직접 못 쓰니 Upload 힙을 거쳐 복사한다
static void CreateMeshInDefaultHeap(const Settings& s, uint64_t size)
{
    HRESULT hr = S_OK;
    g_meshStatic = CreateBuffer(HK_DEFAULT, size, false, &hr);
    if (!g_meshStatic) { g_meshError = "Default 힙 버퍼 생성 실패"; return; }
    if (!s.meshReupload)
    {
        ResPtr staging = CreateBuffer(HK_UPLOAD, size, true, &hr);
        if (!staging) { g_meshError = "Upload 힙(임시) 버퍼 생성 실패"; return; }
        memcpy(staging->mapped, g_meshCpu.data(), size);
        // ===== [시나리오 1] 최적: Upload(임시) -> Default(VRAM)로 복사 큐에서 딱 한 번 복사 =====
        g_meshUploadMs = RunOnCopyQueue([&](ID3D12GraphicsCommandList* cl) {
            cl->CopyBufferRegion(g_meshStatic->res.Get(), 0, staging->res.Get(), 0, size);
        });
        // staging은 여기서 사라진다 -> 시스템 RAM 반환
    }
    else
    {
        for (auto& f : g_frames)   // 프레임마다 쓸 임시 Upload 버퍼
        {
            f.meshFrame = CreateBuffer(HK_UPLOAD, size, true, &hr);
            if (!f.meshFrame) { g_meshError = "프레임별 Upload 버퍼 생성 실패"; return; }
        }
    }
}

// Upload 힙 / GPU Upload 힙: CPU가 바로 memcpy 할 수 있는 힙
static void CreateMeshInCpuWritableHeap(const Settings& s, HeapKind kind, uint64_t size)
{
    HRESULT hr = S_OK;
    if (!s.meshReupload)
    {
        g_meshStatic = CreateBuffer(kind, size, true, &hr);
        if (!g_meshStatic) { g_meshError = kind == HK_GPU_UPLOAD ? "GPU Upload 힙 생성 실패 (ReBAR 지원 확인)" : "Upload 힙 버퍼 생성 실패"; return; }
        double t0 = NowMs();
        memcpy(g_meshStatic->mapped, g_meshCpu.data(), size);  // CPU가 직접 씀 (복사 명령 없음)
        g_meshUploadMs = NowMs() - t0;
    }
    else
    {
        for (auto& f : g_frames)   // GPU가 아직 읽는 중일 수 있으니 프레임마다 따로
        {
            f.meshFrame = CreateBuffer(kind, size, true, &hr);
            if (!f.meshFrame) { g_meshError = "프레임별 버퍼 생성 실패"; return; }
        }
    }
}

static void RebuildMesh(const Settings& s)
{
    WaitIdle();
    g_meshStatic.reset();
    for (auto& f : g_frames) f.meshFrame.reset();
    g_meshError.clear();
    g_meshUploadMs = 0.0;
    if (s.meshTrisK != g_meshGenTrisK) GenerateSphere(s.meshTrisK);
    g_meshBuiltHeap = s.meshHeap; g_meshBuiltTrisK = s.meshTrisK; g_meshBuiltReupload = s.meshReupload;
    if (g_meshCpu.empty()) return;

    uint64_t size = g_meshCpu.size() * sizeof(Vertex);
    HeapKind kind = s.meshHeap == MH_UPLOAD ? HK_UPLOAD : s.meshHeap == MH_DEFAULT ? HK_DEFAULT : HK_GPU_UPLOAD;
    if (s.meshHeap == MH_DEFAULT) CreateMeshInDefaultHeap(s, size);
    else CreateMeshInCpuWritableHeap(s, kind, size);
}

// ---------------------------------------------------------------------
// 스왑체인 / 렌더 타깃 / 깊이
// ---------------------------------------------------------------------
static void CreateSizeDependent()
{
    for (UINT i = 0; i < g_bbCount; i++)
    {
        HR(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g_rtvInc;
        g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, h);
    }
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; dd.Width = g_width; dd.Height = g_height;
    dd.DepthOrArraySize = 1; dd.MipLevels = 1; dd.Format = kDepthFormat; dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;   // SRV 금지 플래그를 안 넣음 -> 배리어 실험에 사용
    D3D12_CLEAR_VALUE cv{}; cv.Format = kDepthFormat; cv.DepthStencil.Depth = 1.0f;
    g_depth = CreateTracked(HK_DEFAULT, dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, false);
    if (!g_depth) Fatal("Create depth", E_FAIL, __FILE__, __LINE__);
    g_device->CreateDepthStencilView(g_depth->res.Get(), nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // 화면 되읽기용 Readback 버퍼
    D3D12_RESOURCE_DESC bd = g_backBuffers[0]->GetDesc();
    UINT rows; UINT64 rowBytes;
    g_device->GetCopyableFootprints(&bd, 0, 1, 0, &g_rbFootprint, &rows, &rowBytes, &g_rbBytes);
    for (auto& f : g_frames) { f.sceneReadback.reset(); f.rbValid = false; }
}

static void ReleaseSizeDependent()
{
    for (auto& b : g_backBuffers) b.Reset();
    g_depth.reset();
    for (auto& f : g_frames) f.sceneReadback.reset();
}

// ===== [시나리오 8] 백 버퍼 개수(2/3장)를 바꾸는 곳 =====
static void ResizeSwapChain(UINT w, UINT h, UINT count)
{
    WaitIdle();
    ReleaseSizeDependent();
    g_width = w; g_height = h; g_bbCount = count;
    HR(g_swapChain->ResizeBuffers(count, w, h, kBackBufferFormat, g_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    CreateSizeDependent();
}

// ---------------------------------------------------------------------
// VRAM 압박 실험
// ---------------------------------------------------------------------
static void SetPressureResidency(bool evict);
// ===== [시나리오 9] 128MB 덩어리를 원하는 만큼 만들거나 지운다 =====
static void ApplyPressure(const Settings& s)
{
    if (g_pressureEvicted) SetPressureResidency(false);   // Evict는 횟수를 센다 -> 먼저 짝을 맞춰 되돌림
    int want = (int)(((uint64_t)s.pressureMB << 20) / kPressureChunk);
    if (s.pressureHeap != g_pressureBuiltHeap || want < (int)g_pressure.size())
    {
        WaitIdle();
        if (s.pressureHeap != g_pressureBuiltHeap) g_pressure.clear();
        while ((int)g_pressure.size() > want) g_pressure.pop_back();
        g_pressureBuiltHeap = s.pressureHeap;
    }
    g_pressureError.clear();
    while ((int)g_pressure.size() < want)
    {
        HRESULT hr;
        ResPtr r = CreateBuffer(s.pressureHeap == 0 ? HK_DEFAULT : HK_UPLOAD, kPressureChunk, false, &hr);
        if (!r)
        {
            char b[128]; snprintf(b, sizeof b, "%dMB에서 할당 실패 (0x%08X) - 더 이상 메모리를 못 줌", (int)(g_pressure.size() * 128), (unsigned)hr);
            g_pressureError = b; break;
        }
        g_pressure.push_back(std::move(r));
    }
    g_pressureEvicted = false;
}

// ===== [시나리오 9] Evict / MakeResident: VRAM에서 빼 두기 / 다시 넣기 =====
static void SetPressureResidency(bool evict)
{
    if (g_pressure.empty()) { g_pressureEvicted = false; return; }
    WaitIdle();
    std::vector<ID3D12Pageable*> p;
    for (auto& r : g_pressure) p.push_back(r->res.Get());
    if (evict) HR(g_device->Evict((UINT)p.size(), p.data()));
    else       HR(g_device->MakeResident((UINT)p.size(), p.data()));
    g_pressureEvicted = evict;
}

// ---------------------------------------------------------------------
// 메모리 정보 읽기
// ---------------------------------------------------------------------
// ===== [메모리 표시] VRAM, 공유 GPU 메모리, 시스템 RAM, 프로세스 RAM을 OS에 물어보는 곳 =====
static void QueryMemory()
{
    g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &g_mem.local);
    g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &g_mem.nonLocal);
    MEMORYSTATUSEX ms{ sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    g_mem.sysTotal = ms.ullTotalPhys; g_mem.sysAvail = ms.ullAvailPhys;
    PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof(pm);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    g_mem.workingSet = pm.WorkingSetSize; g_mem.privateBytes = pm.PrivateUsage;
}

// ---------------------------------------------------------------------
// 초기화
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// 초기화 (DirectX-Graphics-Samples 처럼 LoadPipeline = 장치/큐/스왑체인, LoadAssets = 그릴 재료)
// ---------------------------------------------------------------------
// 1) 장치(Device): 고성능 GPU를 골라 만든다
static void CreateDevice()
{
    UINT factoryFlags = 0;
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); factoryFlags |= DXGI_CREATE_FACTORY_DEBUG; }
    }
#endif
    HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&g_factory)));

    // 고성능 GPU(외장 그래픽) 우선 선택
    ComPtr<IDXGIAdapter1> a1;
    for (UINT i = 0; g_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a1)) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 d; a1->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { a1.Reset(); continue; }
        if (SUCCEEDED(D3D12CreateDevice(a1.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)))) break;
        a1.Reset();
    }
    if (!g_device) Fatal("D3D12CreateDevice (DX12 지원 GPU가 없음)", E_FAIL, __FILE__, __LINE__);
    HR(a1.As(&g_adapter));
    {
        DXGI_ADAPTER_DESC1 d; a1->GetDesc1(&d);
        char name[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        g_adapterName = name; g_dedicatedVram = d.DedicatedVideoMemory; g_sharedSys = d.SharedSystemMemory;
    }
}

// 2) 이 GPU가 무엇을 지원하는지 확인 (ReBAR, UMA, 티어링)
static void QueryFeatures()
{
    // 기능 확인: GPU Upload Heap(ReBAR), UMA(통합 그래픽)
    FeatureOptions16 o16{};
    if (SUCCEEDED(g_device->CheckFeatureSupport(kFeatureOptions16, &o16, sizeof(o16)))) g_gpuUploadSupported = o16.GPUUploadHeapSupported != FALSE;
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    if (SUCCEEDED(g_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch)))) { g_uma = arch.UMA != FALSE; g_cacheCoherentUma = arch.CacheCoherentUMA != FALSE; }
    {
        BOOL tearing = FALSE;
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(g_factory.As(&f5)) && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) g_tearingSupported = tearing != FALSE;
    }
}

// 3) 명령 큐(그리기용 + 복사 전용), 펜스, 복사용 명령 목록
static void CreateQueuesAndFences()
{
    // 큐: 그리기용(Direct) + 복사 전용(Copy)
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_directQueue)));
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    HR(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_copyQueue)));
    HR(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    HR(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copyFence)));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HR(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_copyAlloc)));
    HR(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_copyAlloc.Get(), nullptr, IID_PPV_ARGS(&g_copyList)));
    HR(g_copyList->Close());
    HR(g_directQueue->GetTimestampFrequency(&g_tsFreq));
}

// 4) 스왑체인: 화면에 번갈아 내보낼 백 버퍼들
static void CreateSwapChain()
{
    // 스왑체인
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_width = std::max<UINT>(1, rc.right - rc.left); g_height = std::max<UINT>(1, rc.bottom - rc.top);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_width; sd.Height = g_height; sd.Format = kBackBufferFormat; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = g_bbCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    HR(g_factory->CreateSwapChainForHwnd(g_directQueue.Get(), g_hwnd, &sd, nullptr, nullptr, &sc1));
    HR(sc1.As(&g_swapChain));
    g_factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
}

// 5) 디스크립터 힙: RTV / DSV / SRV(셰이더가 보는 이름표, ImGui와 공유)
static void CreateDescriptorHeaps()
{
    // 디스크립터 힙: RTV, DSV, 셰이더가 보는 SRV 힙(ImGui와 공유)
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kMaxBackBuffers;
    HR(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    HR(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dsvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 64; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)));
    g_rtvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_srvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srvAlloc.Init(64);
}

// 6) 프레임 자원: 프레임마다 따로 쓰는 명령 할당자, 상수 버퍼, 되읽기 버퍼
static void CreateFrameResources()
{
    // 프레임 자원
    for (auto& f : g_frames)
    {
        HR(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)));
        f.frameCB = MustBuffer(HK_UPLOAD, sizeof(FrameCB), true);
        f.inst = MustBuffer(HK_UPLOAD, sizeof(Instance) * kMaxObjects, true);
        f.tsReadback = MustBuffer(HK_READBACK, sizeof(uint64_t) * kTsPerFrame, true);
        f.statsReadback = MustBuffer(HK_READBACK, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), true);
    }
    HR(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc.Get(), nullptr, IID_PPV_ARGS(&g_list)));
    HR(g_list->Close());
}

// 7) 쿼리 힙: GPU 시간(타임스탬프)과 GPU가 센 정점/삼각형 수
static void CreateQueryHeaps()
{
    D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = kTsPerFrame * kMaxFrames;
    HR(g_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_queryHeap)));
    D3D12_QUERY_HEAP_DESC ph{}; ph.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; ph.Count = kMaxFrames;
    if (FAILED(g_device->CreateQueryHeap(&ph, IID_PPV_ARGS(&g_pipeStatsHeap)))) g_pipeStatsHeap.Reset();
}

// 장치부터 디스크립터 힙까지: 어떤 DX12 프로그램이든 똑같은 뼈대
static void LoadPipeline()
{
    CreateDevice();
    QueryFeatures();
    CreateQueuesAndFences();
    CreateSwapChain();
    CreateDescriptorHeaps();
    CreateFrameResources();
    CreateQueryHeaps();
}

// 이 데모가 그릴 재료: 깊이 버퍼, 루트 시그니처, 셰이더, PSO, 큐브, 텍스처
static void LoadAssets()
{
    CreateSizeDependent();
    CreateRootSignature();
    if (!CompileShaders(g_vsBlob, g_psBlob)) ExitProcess(1);
    g_psoA = CreatePso(g_vsBlob.Get(), g_psBlob.Get(), 0);
    g_psoB = CreatePso(g_vsBlob.Get(), g_psBlob.Get(), 1, false, true);
    g_psoWireA = CreatePso(g_vsBlob.Get(), g_psBlob.Get(), 0, true);
    g_psoWireB = CreatePso(g_vsBlob.Get(), g_psBlob.Get(), 1, true, true);
    CreateCube();
    RebuildTexture(g_s);
    g_pressureSink = MustBuffer(HK_DEFAULT, 64 * 1024, false);
}

static void InitD3D()
{
    LoadPipeline();
    LoadAssets();
}

// VRAM 압박 실험의 안전 상한 (MB)
static int PressureMaxMB(int heap)
{
    int64_t mine = (int64_t)g_pressure.size() * (int64_t)(kPressureChunk >> 20);
    int64_t m;
    if (heap == 0) m = (int64_t)(g_mem.local.Budget >> 20) + 1024;                    // VRAM 예산 + 1GB까지만
    else m = std::min<int64_t>((int64_t)(g_mem.nonLocal.Budget >> 20),
                               (int64_t)(g_mem.sysAvail >> 20) + (g_pressureBuiltHeap == 1 ? mine : 0) - 3072); // 시스템 RAM 3GB는 남김
    m = std::max<int64_t>(128, m);
    return (int)(m / 128 * 128);
}

// ---------------------------------------------------------------------
// 도움말 툴팁
// ---------------------------------------------------------------------
// (?) 도움말: 마우스를 올리면 툴팁, 클릭하면 패널 안에 펼쳐 둔다 (툴팁이 화면 밖으로 잘릴 때 대비)
static ImGuiStorage g_helpOpen;
static void Help(const char* text)
{
    ImGui::SameLine();
    ImGuiID id = ImGui::GetID(text);
    bool open = g_helpOpen.GetBool(id, false);
    if (open) ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1), "(접기)");
    else      ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) { open = !open; g_helpOpen.SetBool(id, open); }
    if (!open && ImGui::BeginItemTooltip())
    {
        // 화면 폭에 맞춰 줄바꿈 폭을 정한다 -> 넓으면 낮고 넓게, 좁으면 좁게. ImGui가 툴팁을 화면 안으로 옮겨 준다.
        float wrap = std::min(ImGui::GetFontSize() * 38.0f, ImGui::GetIO().DisplaySize.x * 0.7f);
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted(text);
        ImGui::TextDisabled("(클릭하면 패널 안에 펼쳐 둡니다)");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (open)
    {
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.86f, 0.95f, 1));
        ImGui::PushTextWrapPos(0.0f);   // 패널 오른쪽 끝에서 줄바꿈
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::Spacing();
    }
}
static void Verdict(bool good, const char* goodText, const char* badText)
{
    if (good) ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "  최적: %s", goodText);
    else      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "  최악: %s", badText);
}
// 설정 옆에 "이 설정이 바꾸는 코드 변수"를 보여 준다. 패널 위 "코드 변수 이름 보기"로 끄고 켠다.
// var  = 바뀌는 변수 (s 는 Settings 구조체 g_s. 패널과 프레임 함수들 모두 s 라는 이름으로 씀)
// where = 그 변수를 실제로 쓰는 곳. [시나리오 N] 을 Ctrl+F로 찾으면 그 코드로 간다.
static bool g_showCodeVars = true;
static bool g_showGraphOverlay = true;   // 렌더 화면 오른쪽 위에 CPU/GPU 그래프
static void CodeVar(const char* var, const std::string& value, const char* where)
{
    if (!g_showCodeVars) return;
    ImGui::TextColored(ImVec4(0.55f, 0.78f, 0.72f, 1), "  코드: %s = %s", var, value.c_str());
    bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    // 오른쪽에 자리가 있으면 같은 줄에, 없으면 다음 줄에 "어디서 쓰는지"를 적는다
    char w[256]; snprintf(w, sizeof w, "· %s", where);
    float need = ImGui::CalcTextSize(w).x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < need) { ImGui::NewLine(); ImGui::TextDisabled("      %s", w); }
    else ImGui::TextDisabled("%s", w);
    hovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    if (hovered && ImGui::BeginTooltip())
    {
        ImGui::Text("변수: %s (지금 값: %s)", var, value.c_str());
        ImGui::Text("쓰는 곳: %s", where);
        ImGui::TextDisabled("s = Settings 구조체 g_s (main.cpp의 struct Settings)");
        ImGui::TextDisabled("Visual Studio에서 Ctrl+F로 변수 이름이나 [시나리오 N]을 찾아 보세요");
        ImGui::EndTooltip();
    }
}
static std::string CV(int v) { return std::to_string(v); }
static std::string CV(bool v) { return v ? "true" : "false"; }
static const char* kMeshHeapName[] = { "MH_UPLOAD", "MH_DEFAULT", "MH_GPU_UPLOAD" };
static const char* kBufferModeName[] = { "BM_REUSE", "BM_NEW_BIG", "BM_NEW_PER_OBJECT" };
static const char* kReadbackName[] = { "RB_OFF", "RB_WAIT_NOW", "RB_DELAYED" };
static std::string Num(uint64_t n)   // 1234567 -> "1,234,567"
{
    std::string d = std::to_string(n), r;
    for (size_t i = 0; i < d.size(); i++) { if (i && (d.size() - i) % 3 == 0) r += ','; r += d[i]; }
    return r;
}
static std::string MB(uint64_t b) { char s[64]; snprintf(s, sizeof s, "%.0f MB", b / 1048576.0); return s; }
static std::string GB(uint64_t b) { char s[64]; snprintf(s, sizeof s, "%.2f GB", b / 1073741824.0); return s; }
static void MemBar(const char* label, uint64_t used, uint64_t total, const char* help)
{
    ImGui::TextUnformatted(label); Help(help);
    char o[96]; snprintf(o, sizeof o, "%s / %s", MB(used).c_str(), MB(total).c_str());
    float frac = total ? (float)((double)used / (double)total) : 0.0f;
    ImVec4 col = frac < 0.75f ? ImVec4(0.3f, 0.65f, 1, 1) : frac < 0.95f ? ImVec4(1, 0.75f, 0.2f, 1) : ImVec4(1, 0.3f, 0.3f, 1);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(std::min(frac, 1.0f), ImVec2(-FLT_MIN, 0), o);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------
// 패널 UI
// ---------------------------------------------------------------------
// 프리셋
static void PanelPresets(Settings& s)
{
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.18f, 0.18f, 1));
    if (ImGui::Button("전부 최악 (F1)", ImVec2(bw, 0))) PresetWorst(s);
    ImGui::PopStyleColor(); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.5f, 0.25f, 1));
    if (ImGui::Button("전부 최적 (F2)", ImVec2(bw, 0))) PresetBest(s);
    ImGui::PopStyleColor(); ImGui::SameLine();
    if (ImGui::Button("패널 숨김 (F3)", ImVec2(bw, 0))) g_panelHidden = true;
    ImGui::SliderFloat("글자 크기", &ImGui::GetStyle().FontScaleMain, 0.8f, 2.0f, "%.1f배");
    Help("발표장 프로젝터에서 잘 보이게 글자를 키운다.");
}

// 성능
static void PanelPerformance()
{
    if (!ImGui::CollapsingHeader("성능", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SliderFloat("평균 구간", &g_avgSeconds, 0.5f, 10.0f, "%.1f초");
    Help("FPS와 시간을 몇 초 동안 평균 낼지 정한다. 순간 값은 너무 빨리 바뀌어서 읽기 어렵기 때문.\n"
         "설정을 바꾼 직후에는 '평균 다시 모으기'(F5)를 누르면 이전 설정 값이 섞이지 않는다.");
    if (ImGui::Button("평균 다시 모으기 (F5)")) ResetAverage(NowMs());
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f / %.1f초 모음", g_avg.covered, g_avgSeconds);
    if (ImGui::Checkbox("와이어프레임 보기 (F4)", &g_wireframe)) ResetAverage(NowMs());
    Help("메시를 면 대신 삼각형 테두리 선으로 그린다. PSO의 FillMode를 WIREFRAME으로 바꾼 것.\n"
         "쉽게 말하면 완성된 조각상 대신 철사로 만든 뼈대만 보는 것. 행성이 얼마나 촘촘한 삼각형으로 되어 있는지 보인다.\n"
         "PSO는 미리 두 벌(면/와이어) 만들어 두고 고르기만 해서 전환 비용이 없다.");
    ImGui::Checkbox("코드 변수 이름 보기", &g_showCodeVars);
    Help("각 설정 아래에 '코드: s.변수이름 = 지금 값 · [시나리오 N] 쓰는 곳'을 보여 준다.\n"
         "설정을 움직이면 바로 그 변수 값이 바뀌고, 오른쪽에 적힌 곳에서 그 값을 읽어 동작이 달라진다.\n"
         "발표 때 화면을 깔끔하게 하려면 꺼 두면 된다.");
    ImGui::Checkbox("화면 오른쪽 위에 그래프 보기", &g_showGraphOverlay);
    Help("렌더 화면 오른쪽 위에 CPU 프레임 시간과 GPU 시간 그래프를 띄운다. 패널을 숨겨도(F3) 보인다.\n"
         "세로 범위는 최근 값에 맞춰 자동으로 바뀌고, 점선은 60 FPS(16.7ms) 기준선이다.\n"
         "설정을 바꾸는 순간 선이 튀어 오르거나 내려가는 걸 청중이 바로 볼 수 있다.");
    ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), "%.1f초 평균: FPS %.1f   프레임 %.2f ms   GPU %.2f ms", g_avgSeconds, g_avg.fps, g_avg.frame, g_avg.gpu);
    ImGui::Text("평균 CPU: 대기 %.2f / 작업 %.2f / Present %.2f ms", g_avg.wait, g_avg.work, g_avg.present);
    ImGui::TextDisabled("순간 값: FPS %.0f   프레임 %.2f ms", g_stats.frameMs > 0 ? 1000.0 / g_stats.frameMs : 0.0, g_stats.frameMs);
    ImGui::PlotLines("##ft", g_histFrame, kHist, g_histPos, "CPU 프레임 시간(ms)", 0.0f, 50.0f, ImVec2(-FLT_MIN, 60));
    ImGui::PlotLines("##gt", g_histGpu, kHist, g_histPos, "GPU 시간(ms)", 0.0f, 50.0f, ImVec2(-FLT_MIN, 60));
    ImGui::Text("CPU: 대기 %.2f / 작업 %.2f / Present %.2f ms", g_stats.waitMs, g_stats.cpuWorkMs, g_stats.presentMs);
    Help("대기 = 펜스로 GPU를 기다린 시간 (GPU가 느리거나 동기화가 과할 때 커짐)\n"
         "작업 = 데이터 복사, 리소스 생성, 명령 기록 등 CPU가 일한 시간\n"
         "Present = 화면 교체 요청에 걸린 시간 (VSync를 켜면 여기서 기다림)");
    ImGui::Text("GPU: 전체 %.2f / 행성 %.2f / 큐브 %.2f ms", g_stats.gpuTotalMs, g_stats.gpuMeshMs, g_stats.gpuCubesMs);
    Help("GPU 타임스탬프 쿼리로 잰 실제 GPU 작업 시간.\n쉽게 말하면 GPU 안에 스톱워치를 넣어 두고 구간마다 누른 것.");
    ImGui::Text("드로우콜 %d  배리어 %d  PSO설정 %d  루트시그설정 %d", g_stats.drawCalls, g_stats.barriers, g_stats.psoSets, g_stats.rootSigSets);
    ImGui::SeparatorText("정점 / 삼각형 수");
    ImGui::Text("화면의 물체: 정점 %s · 삼각형 %s", Num(g_stats.uniqueVerts).c_str(), Num(g_stats.uniqueTris).c_str());
    Help("지금 화면에 있는 물체(행성 + 큐브)의 모양 데이터에 들어 있는 정점 수. 각 물체를 1번씩 센 값.\n"
         "쉽게 말하면 레고 작품들에 쓰인 블록 수.\n"
         "행성은 인덱스 없이 삼각형마다 정점 3개, 큐브는 정점 24개(면 6개 x 꼭짓점 4개)와 삼각형 12개.");
    ImGui::Text("이번 프레임 보낸 양: 정점 %s · 삼각형 %s", Num(g_stats.submittedVerts).c_str(), Num(g_stats.submittedTris).c_str());
    Help("GPU에 '그려라'고 보낸 정점 수. '같은 자리에 반복 그리기'를 N으로 하면 행성 몫이 N배가 된다.\n"
         "큐브는 인덱스 36개를 보내므로 큐브 1개당 36으로 센다 (같은 꼭짓점을 여러 삼각형이 같이 씀).\n"
         "쉽게 말하면 블록 수가 아니라 '블록을 집어 든 횟수'.");
    if (g_stats.gpuStatsValid)
    {
        ImGui::Text("GPU가 센 값: 정점 셰이더 %s회 · 래스터 삼각형 %s", Num(g_stats.gpuVSInvocations).c_str(), Num(g_stats.gpuRasterPrims).c_str());
        Help("파이프라인 통계 쿼리(D3D12_QUERY_TYPE_PIPELINE_STATISTICS)로 GPU가 직접 센 숫자. 몇 프레임 늦게 도착한다.\n"
             "정점 셰이더 횟수: 실제로 정점 셰이더가 돈 횟수. 인덱스로 같은 꼭짓점을 다시 쓰면 GPU가 결과를 재사용(정점 캐시)해서\n"
             "'보낸 정점'보다 적게 나올 수 있다.\n"
             "래스터 삼각형: 화면 밖으로 잘린 것 등을 빼고 실제로 픽셀로 바뀐 삼각형 수.");
        ImGui::Text("픽셀 셰이더 %s회", Num(g_stats.gpuPSInvocations).c_str());
        Help("픽셀(정확히는 픽셀 후보) 하나를 칠할 때마다 1회. 반복 그리기에서 두 번째부터는 깊이 테스트에 떨어져서\n"
             "이 숫자는 거의 안 늘고 정점 셰이더 횟수만 늘어난다 -> 1번 항목의 실험 원리를 숫자로 확인할 수 있다.");
    }
    ImGui::Text("이번 프레임에 만든 리소스 %d개", g_stats.created);
}

// 메모리
static void PanelMemory()
{
    if (!ImGui::CollapsingHeader("메모리 (VRAM / 시스템 RAM)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("GPU: %s", g_adapterName.c_str());
    ImGui::TextWrapped("전용 VRAM %s · ReBAR(GPU Upload Heap): %s · %s", GB(g_dedicatedVram).c_str(),
                g_gpuUploadSupported ? "지원" : "미지원", g_uma ? "통합 그래픽(UMA)" : "외장 그래픽");
    Help("ReBAR(Resizable BAR): CPU가 VRAM 전체를 직접 쓸 수 있게 문을 넓혀 주는 기능.\n"
         "쉽게 말하면 창고(VRAM) 쪽문이 256MB짜리 작은 창문에서 통째로 열리는 대문이 되는 것.\n"
         "미지원이면: BIOS에서 Resizable BAR / Above 4G Decoding 켜기, 최신 드라이버, Windows 11 확인.\n\n"
         "UMA: CPU와 GPU가 같은 메모리를 쓰는 구조(노트북 내장 그래픽). 이 경우 VRAM이 따로 없다.");
    MemBar("VRAM (Local) 사용 / 예산", g_mem.local.CurrentUsage, g_mem.local.Budget,
           "우리 프로세스가 GPU 전용 메모리(VRAM)를 얼마나 쓰는지 / OS가 우리에게 허락한 한도.\n"
           "쉽게 말하면 '우리 반에 배정된 사물함 칸 수' 중 몇 칸 쓰는지.\n"
           "예산을 넘으면 OS가 일부를 시스템 RAM으로 쫓아낸다(느려짐).\n(DXGI QueryVideoMemoryInfo LOCAL)");
    ImGui::PlotLines("##vram", g_histVram, kHist, g_histPos, "VRAM MB", 0.0f, (float)(g_mem.local.Budget >> 20) + 1.0f, ImVec2(-FLT_MIN, 40));
    MemBar("공유 GPU 메모리 (Non-local) 사용 / 예산", g_mem.nonLocal.CurrentUsage, g_mem.nonLocal.Budget,
           "GPU가 쓰는 메모리 중 시스템 RAM에 있는 부분. Upload/Readback 힙이 여기에 잡힌다.\n"
           "쉽게 말하면 학교 사물함이 모자라서 집 창고에 둔 짐. GPU가 가지러 가려면 PCIe라는 먼 길을 가야 한다.\n(DXGI QueryVideoMemoryInfo NON_LOCAL)");
    MemBar("시스템 RAM 사용 / 전체", g_mem.sysTotal - g_mem.sysAvail, g_mem.sysTotal,
           "컴퓨터 전체의 RAM 사용량 (다른 프로그램 포함). GlobalMemoryStatusEx");
    ImGui::PlotLines("##ram", g_histRam, kHist, g_histPos, "이 프로그램 Private MB", 0.0f, 0.0f, ImVec2(-FLT_MIN, 40));
    ImGui::Text("이 프로그램: Working Set %s · Private %s", MB(g_mem.workingSet).c_str(), MB(g_mem.privateBytes).c_str());
    Help("Working Set: 지금 실제 RAM에 올라와 있는 우리 프로그램의 메모리.\n"
         "Private Bytes: 우리 프로그램만 쓰려고 예약(커밋)한 메모리 총량.\n"
         "Upload 힙을 크게 만들면 이 숫자도 같이 오른다 -> Upload 힙이 시스템 RAM이라는 증거!");
    ImGui::SeparatorText("우리 앱이 직접 만든 GPU 리소스");
    float col2 = ImGui::GetContentRegionAvail().x * 0.66f;
    for (int k = 0; k < HK_COUNT; k++)
    {
        ImGui::TextUnformatted(kHeapKindName[k]); ImGui::SameLine(col2);
        ImGui::Text("%s (%d개)", MB((uint64_t)std::max<int64_t>(0, g_heapBytes[k])).c_str(), g_heapCount[k]);
    }
    ImGui::TextUnformatted("CPU 원본 메시 (std::vector)"); ImGui::SameLine(col2);
    ImGui::TextUnformatted(MB(g_meshCpu.size() * sizeof(Vertex)).c_str());
    Help("발표의 첫 정거장: 데이터는 처음에 평범한 C++ 메모리(시스템 RAM)에 있다.");
}

// 1. 정점 데이터 위치
static void PanelScenario1_MeshHeap(Settings& s)
{
    if (!ImGui::CollapsingHeader("1. 정점 데이터 위치 (슬라이드 5-8, 10-11)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SliderInt("행성 삼각형 (천 개)", &s.meshTrisK, 0, 2000);
    Help("가운데 행성의 삼각형 수. 정점 1개 = 32바이트, 삼각형 1개 = 정점 3개.\n값을 놓는 순간 메시를 다시 만든다.");
    CodeVar("s.meshTrisK", CV(s.meshTrisK), "[시나리오 1] RebuildMesh() -> GenerateSphere()");
    ImGui::SliderInt("같은 자리에 반복 그리기", &s.meshRepeat, 1, 8);
    Help("같은 행성을 똑같은 위치에 N번 겹쳐 그린다 (화면에는 1개로 보임).\n"
         "왜? 이 항목은 'GPU가 정점 데이터를 어디서 읽느냐'를 재려는 실험이다. 행성 1번이면 차이가 작아서 잘 안 보인다.\n"
         "두 번째부터는 이미 그린 행성과 깊이가 같아서 깊이 테스트(LESS)에 떨어진다 -> 픽셀 칠하기는 거의 생략된다.\n"
         "하지만 정점 셰이더는 매번 돌아서 정점 데이터를 버퍼에서 다시 읽는다.\n"
         "그래서 N을 올리면 '정점 읽기' 비용만 N배가 되고, Upload 힙(PCIe 너머)과 Default 힙(VRAM)의 차이가 N배로 커진다.\n"
         "쉽게 말하면 같은 재료를 N번 가지러 가게 해서, 냉장고가 집에 있느냐 주방에 있느냐의 차이를 확대해 보는 것.");
    CodeVar("s.meshRepeat", CV(s.meshRepeat), "[시나리오 1] DrawPlanet(): DrawInstanced 반복");
    ImGui::TextUnformatted("행성 정점 버퍼를 둘 힙:");
    Help("여기서 말하는 버퍼는 화면(백 버퍼)이 아니라 '행성 모양 데이터'다.\n"
         "= 삼각형 꼭짓점들의 위치, 법선, UV를 담은 정점 버퍼(Vertex Buffer). GPU가 그릴 때 읽는 재료.\n"
         "화면에 보이는 그림이 담기는 버퍼(백 버퍼)는 스왑체인이 만들고 항상 VRAM에 있다 (8번 항목).\n"
         "즉 이 항목은 '재료 창고를 어디에 둘까'이고, 8번은 '완성된 그림을 걸어 둘 액자'다.");
    ImGui::RadioButton("Upload 힙 (시스템 RAM)", &s.meshHeap, MH_UPLOAD);
    Help("Upload Heap: CPU가 쓰기 좋은 시스템 RAM 영역. GPU는 PCIe 너머로 읽어야 해서 느리다.\n"
         "쉽게 말하면 재료를 우리 집 냉장고에 두고, 요리사(GPU)가 요리할 때마다 차 타고 가지러 오는 것.");
    ImGui::RadioButton("Default 힙 (VRAM)", &s.meshHeap, MH_DEFAULT);
    Help("Default Heap: GPU 바로 옆 VRAM. CPU는 직접 못 쓰고, Upload 힙에 올린 뒤 복사 명령으로 옮긴다.\n"
         "쉽게 말하면 재료를 한 번 식당 주방 냉장고로 옮겨 두면 요리사가 손만 뻗으면 된다.\n"
         "이 데모는 복사 전용 큐(Copy Queue)로 옮기고 펜스로 끝을 기다린다.");
    ImGui::BeginDisabled(!g_gpuUploadSupported);
    ImGui::RadioButton("GPU Upload 힙 (VRAM, ReBAR)", &s.meshHeap, MH_GPU_UPLOAD);
    ImGui::EndDisabled();
    Help("GPU Upload Heap: ReBAR 덕분에 CPU가 VRAM에 직접 쓰는 힙 (D3D12_HEAP_TYPE_GPU_UPLOAD).\n"
         "쉽게 말하면 주방 냉장고에 우리 집에서 바로 넣을 수 있는 배달 창구가 생긴 것. 복사 단계가 없다.\n"
         "회색이면 이 PC는 지원하지 않는 것 (ReBAR 꺼짐 / 드라이버 / OS).");
    CodeVar("s.meshHeap", kMeshHeapName[s.meshHeap], "[시나리오 1] RebuildMesh()");
    ImGui::Checkbox("매 프레임 CPU에서 다시 보내기", &s.meshReupload);
    Help("매 프레임 CPU에 있는 원본(std::vector) 전체를 GPU 쪽 버퍼로 다시 memcpy 한다.\n\n"
         "정적인(static) 메시 = 정점 데이터 자체가 안 바뀌는 메시. '가만히 서 있는 물체'라는 뜻이 아니다!\n"
         "행성은 빙글빙글 돌지만, 도는 건 셰이더에 넘기는 회전 행렬(상수 몇 바이트)이 바꾸는 것이고 꼭짓점 좌표 데이터는 그대로다.\n"
         "그러니 한 번만 VRAM에 올리면 끝. 매 프레임 다시 보내는 건 낭비다.\n"
         "반대로 동적(dynamic) 메시 = 꼭짓점 자체가 매 프레임 바뀌는 것 (천 시뮬레이션, CPU 스키닝, 파티클, 지형 변형).\n"
         "이런 것만 매 프레임 보낼 가치가 있다.\n"
         "쉽게 말하면 레고 성을 돌려 보려고 매번 새로 조립할 필요는 없다. 받침대만 돌리면 된다.\n\n"
         "켜면: CPU 작업 시간 증가 + 프레임 수만큼 복사본 메모리가 생긴다.");
    CodeVar("s.meshReupload", CV(s.meshReupload), "[시나리오 1] PrepareMeshForFrame(): 매 프레임 memcpy");
    bool good = s.meshHeap != MH_UPLOAD && !s.meshReupload;
    Verdict(good, "VRAM에 한 번 올려 두고 GPU가 가까이서 읽음", s.meshReupload ? "매 프레임 다시 복사" : "GPU가 매번 PCIe 너머 시스템 RAM에서 읽음");
    ImGui::Text("  GPU가 읽는 정점: %s/프레임 · CPU 복사: %s/프레임", MB(g_stats.meshBytesReadPerFrame).c_str(), MB(g_stats.meshBytesCopiedPerFrame).c_str());
    if (g_meshUploadMs > 0) ImGui::Text("  처음 올릴 때 걸린 시간: %.2f ms", g_meshUploadMs);
    if (!g_meshError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "  %s", g_meshError.c_str());
}

// 2. 리소스 생성
static void PanelScenario2_Buffers(Settings& s)
{
    if (!ImGui::CollapsingHeader("2. 리소스 생성 (슬라이드 10)")) return;
    ImGui::RadioButton("미리 만든 큰 버퍼 1개 재사용", &s.bufferMode, BM_REUSE);
    ImGui::RadioButton("매 프레임 큰 버퍼 1개 새로 만들기", &s.bufferMode, BM_NEW_BIG);
    ImGui::RadioButton("매 프레임 물체마다 작은 버퍼 새로 만들기", &s.bufferMode, BM_NEW_PER_OBJECT);
    Help("CreateCommittedResource는 OS와 드라이버에게 메모리를 새로 달라고 하는 비싼 호출이다.\n"
         "쉽게 말하면 택배 보낼 때마다 새 상자를 공장에 주문하는 것. 상자를 여러 번 재사용하면 빠르다.\n\n"
         "게다가 버퍼 하나는 최소 64KB 단위로 잡힌다. 32바이트짜리 데이터도 64KB를 차지 -> 메모리 낭비가 위 표에 그대로 보인다.\n"
         "(물체마다 모드는 큐브를 최대 3000개로 제한하고, 큐브마다 따로 그린다)");
    CodeVar("s.bufferMode", kBufferModeName[s.bufferMode], "[시나리오 2] UploadCubeInstances()");
    Verdict(s.bufferMode == BM_REUSE, "한 번 만들고 계속 재사용", s.bufferMode == BM_NEW_BIG ? "매 프레임 생성/삭제" : "매 프레임 수천 번 생성 + 64KB 낭비");
}

// 3. 배리어
static void PanelScenario3_Barriers(Settings& s)
{
    if (!ImGui::CollapsingHeader("3. 배리어 (슬라이드 12)")) return;
    ImGui::SliderInt("쓸데없는 배리어 왕복", &s.uselessBarriers, 0, 1000);
    Help("[쉽게 말하면] 공사장 신호수. 같은 도로(리소스)를 '포장 공사'에서 '차량 통행'으로 바꾸려면\n"
         "신호수가 깃발을 들고, 포장 작업이 다 끝나 콘크리트가 굳을 때까지 차를 세워 둬야 한다.\n\n"
         "[뜻] Resource Barrier = '이 리소스의 용도(상태)가 바뀐다'고 GPU에 알리는 명령.\n"
         "예: 그림자 맵을 '깊이를 쓰는 용도(DEPTH_WRITE)'로 그린 뒤, '셰이더가 읽는 용도(PIXEL_SHADER_RESOURCE)'로 바꿔야 다음 패스에서 읽을 수 있다.\n\n"
         "[왜 필요한가] GPU는 수천 개 작업을 동시에, 순서 없이 처리하고, 결과를 캐시에 잠깐 두거나 압축해서 저장한다.\n"
         "용도를 바꿀 때 GPU는 ① 앞 작업이 정말 끝날 때까지 기다리고 ② 캐시에 있던 결과를 메모리로 내보내고\n"
         "③ 압축된 데이터를 다른 장치가 읽을 수 있게 풀어야 한다. 배리어가 이 세 가지를 시키는 신호다.\n"
         "안 하면 반쯤 그려진 데이터를 읽는 버그가 난다. 그래서 필요한 곳에는 꼭 넣어야 한다.\n\n"
         "[왜 비싼가] 배리어마다 GPU가 잠깐 멈춘다(파이프라인 비우기). 꼭 필요한 곳에만, 여러 개를 한 번에 모아 보내는 게 정석.\n\n"
         "[이 데모에서] 깊이 버퍼를 '깊이 쓰기 -> 셰이더 읽기 -> 깊이 쓰기'로 쓸데없이 왕복시키고 그 사이에 드로우를 끼운다.\n"
         "실제로는 아무도 셰이더로 읽지 않는데 GPU는 매번 멈추고, 캐시를 비우고, 깊이 압축을 풀었다 다시 묶는다.\n"
         "볼 숫자: '배리어' 개수와 'GPU: 큐브 ms'. (효과는 GPU 제조사마다 다르다)");
    CodeVar("s.uselessBarriers", CV(s.uselessBarriers), "[시나리오 3] RecordUselessBarrier()");
    Verdict(s.uselessBarriers == 0, "꼭 필요한 배리어만", "필요 없는 배리어로 GPU가 계속 멈춤");
}

// 4. 동기화
static void PanelScenario4_Sync(Settings& s)
{
    if (!ImGui::CollapsingHeader("4. CPU-GPU 동기화 (슬라이드 13, 23)")) return;
    ImGui::SliderInt("동시에 진행할 프레임 수", &s.framesInFlight, 1, 3);
    Help("Fence(펜스): GPU가 '여기까지 끝났어'라고 남기는 번호표. CPU는 이 번호를 보고 기다릴지 정한다.\n"
         "1 = 매 프레임 GPU가 끝날 때까지 CPU가 기다림 (CPU와 GPU가 번갈아 놀고 있음)\n"
         "2~3 = CPU가 다음 프레임을 준비하는 동안 GPU는 이전 프레임을 그림 (둘 다 바쁨)\n"
         "쉽게 말하면 1은 손님 한 명 요리가 다 나와야 다음 주문을 받는 식당, 2~3은 주문을 미리 받아 두는 식당.\n"
         "대신 프레임마다 자원(상수 버퍼 등)이 따로 있어야 해서 메모리가 그만큼 더 든다.");
    CodeVar("s.framesInFlight", CV(s.framesInFlight), "[시나리오 4] WaitForFrameSlot(): WaitFence");
    ImGui::SliderInt("게임 로직 흉내 (CPU ms)", &s.cpuLogicMs, 0, 20, "%d ms");
    Help("실제 게임은 매 프레임 CPU가 AI, 물리, 애니메이션 같은 일을 한다. 그 시간을 흉내 내려고 CPU를 일부러 바쁘게 만든다.\n"
         "차이를 크게 보려면 CPU 시간과 GPU 시간을 비슷하게 맞춘다. 예: 여기 8ms + 행성 반복을 늘려 GPU도 8ms쯤.\n"
         "  프레임 수 1: 8 + 8 = 약 16ms (60 FPS)  ← CPU가 끝나야 GPU 시작, GPU가 끝나야 CPU 시작\n"
         "  프레임 수 2: 둘이 겹쳐서 약 8ms (120 FPS)\n"
         "한쪽만 무거우면(예: GPU 1ms) 겹쳐도 아낄 시간이 거의 없어서 차이가 작다.");
    CodeVar("s.cpuLogicMs", CV(s.cpuLogicMs), "[시나리오 4] SimulateGameLogic()");
    Verdict(s.framesInFlight >= 2, "CPU와 GPU가 겹쳐서 일함", "CPU와 GPU가 서로 기다리며 번갈아 쉼");
}

// 5. 드로우콜
static void PanelScenario5_DrawCalls(Settings& s)
{
    if (!ImGui::CollapsingHeader("5. 드로우콜 (슬라이드 20)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SliderInt("큐브 개수", &s.objectCount, 0, kMaxObjects);
    ImGui::Checkbox("인스턴싱 (한 번에 그리기)", &s.instancing);
    Help("Draw Call: CPU가 GPU에게 '이거 그려'라고 하는 명령 하나.\n"
         "인스턴싱 끔 = 큐브마다 명령 1개 (5000개면 5000번)\n"
         "인스턴싱 켬 = '이 큐브를 5000개 그려' 명령 1번. 위치는 GPU가 버퍼에서 각자 읽는다.\n"
         "쉽게 말하면 학생 5000명에게 한 명씩 전화하기 vs 단체 문자 한 번.");
    CodeVar("s.objectCount", CV(s.objectCount), "[시나리오 5] DrawCubes()");
    CodeVar("s.instancing", CV(s.instancing), "[시나리오 5] DrawCubes()");
    Verdict(s.instancing && s.bufferMode != BM_NEW_PER_OBJECT, "인스턴싱으로 몇 번만 그림", "물체마다 드로우콜");
}

// 6. 상태 변경
static void PanelScenario6_State(Settings& s)
{
    if (!ImGui::CollapsingHeader("6. PSO / 루트 시그니처 변경 (슬라이드 17, 부록 PSO)")) return;
    ImGui::Checkbox("드로우마다 상태 전부 다시 설정", &s.resetStatePerDraw);
    Help("PSO(Pipeline State Object): 셰이더, 블렌드, 깊이 설정 등을 한 덩어리로 묶은 '그리기 설정 세트'.\n"
         "Root Signature: 셰이더가 어떤 자료를 어느 칸(b0, t0...)에서 받을지 정한 '좌석 배치표'.\n"
         "켜면 드로우마다 PSO 두 개를 번갈아 바꾸고 루트 시그니처와 모든 바인딩을 다시 건다.\n"
         "쉽게 말하면 학생 한 명 사진 찍을 때마다 조명과 배경을 전부 바꿨다 되돌리는 것.\n\n"
         "PSO B는 알파 블렌딩이 켜진 진짜 다른 상태라(화면은 같음) GPU가 매번 블렌드 장치를 다시 설정해야 한다.\n\n"
         "효과가 크게 보이는 조건:\n"
         " ① 인스턴싱 끄기 (드로우가 1개면 바꿀 일도 1번뿐)\n"
         " ② 큐브 개수 최대 (20000) -> 상태 변경 20000번 x (PSO + 루트 시그니처 + 바인딩 5개 + 디스크립터 힙)\n"
         " ③ 행성 삼각형 0으로 -> GPU가 한가해야 CPU 쪽 비용이 FPS에 그대로 드러난다\n"
         "볼 숫자: 'CPU 작업 ms'. 최신 드라이버는 상태 변경을 꽤 싸게 처리해서 드로우 1개당 비용은 작다. 개수가 많아야 쌓여서 보인다.");
    CodeVar("s.resetStatePerDraw", CV(s.resetStatePerDraw), "[시나리오 6] DrawCubesOneByOne(): 드로우마다 SetPipelineState");
    ImGui::Checkbox("매 프레임 셰이더 컴파일 + PSO 새로 만들기", &s.recompilePso);
    Help("PSO 생성은 드라이버가 셰이더를 GPU 기계어로 바꾸는 무거운 작업이다.\n"
         "그래서 로딩 화면에서 미리 만들어 두는 것이 정석. 게임 중에 만들면 '셰이더 컴파일 끊김'이 생긴다.");
    CodeVar("s.recompilePso", CV(s.recompilePso), "[시나리오 6] PreparePsosForFrame(): D3DCompile + CreatePso");
    Verdict(!s.resetStatePerDraw && !s.recompilePso, "PSO는 미리 만들고 한 번만 설정", s.recompilePso ? "매 프레임 PSO 생성 (끊김의 원인)" : "드로우마다 상태 변경");
}

// 7. Readback
static void PanelScenario7_Readback(Settings& s)
{
    if (!ImGui::CollapsingHeader("7. GPU -> CPU 되읽기 (슬라이드 6)")) return;
    ImGui::RadioButton("안 함", &s.readback, RB_OFF);
    ImGui::RadioButton("매 프레임 화면을 읽고 바로 기다리기", &s.readback, RB_WAIT_NOW);
    ImGui::RadioButton("매 프레임 읽되 몇 프레임 뒤에 확인", &s.readback, RB_DELAYED);
    Help("Readback Heap: GPU 결과를 CPU가 읽을 수 있게 시스템 RAM에 받아 두는 힙.\n"
         "'바로 기다리기'는 복사를 시키자마자 펜스로 GPU가 끝날 때까지 CPU가 멈춘다 -> 겹쳐서 일하기가 깨진다.\n"
         "'몇 프레임 뒤'는 결과를 늦게 받는 대신 아무도 기다리지 않는다.\n"
         "쉽게 말하면 사진관에 필름 맡기고 가게 앞에서 기다리기 vs 다음 주에 찾으러 오기.\n\n"
         "왜 화면에 출력했는데 또 읽나? Present는 그림을 모니터로 보낼 뿐, CPU(게임 코드)는 그 결과를 전혀 모른다.\n"
         "CPU가 GPU 결과를 알아야 할 때 Readback이 필요하다:\n"
         " - 스크린샷 / 녹화 / 사진 모드 저장\n"
         " - 마우스로 클릭한 물체 찾기 (GPU 피킹: 물체 ID를 그린 텍스처에서 마우스 위치 픽셀 읽기)\n"
         " - 자동 노출: 화면 평균 밝기를 읽어 다음 프레임 노출 조절 (이 데모의 '평균색'이 바로 이것)\n"
         " - GPU로 계산한 물리, 파티클, 가림 판정(오클루전 쿼리) 결과를 게임 로직이 쓸 때\n"
         " - GPU 시간 측정: 이 데모의 'GPU ms'도 타임스탬프를 Readback 힙으로 받아 읽은 것");
    CodeVar("s.readback", kReadbackName[s.readback], "[시나리오 7] CopySceneToReadback() + WaitReadbackNow()");
    if (s.readback != RB_OFF)
    {
        ImGui::ColorButton("##rb", ImVec4(g_rbColor[0], g_rbColor[1], g_rbColor[2], 1), 0, ImVec2(18, 18));
        ImGui::SameLine();
        ImGui::Text("되읽은 화면 평균색 (%d프레임 전 결과)", g_rbAgeFrames);
    }
    Verdict(s.readback != RB_WAIT_NOW, s.readback == RB_OFF ? "되읽기 없음" : "늦게 받아서 아무도 안 기다림", "CPU가 GPU를 기다리며 멈춤");
}

// 8. 스왑체인
static void PanelScenario8_SwapChain(Settings& s)
{
    if (!ImGui::CollapsingHeader("8. 스왑체인 / Present (슬라이드 23-24)")) return;
    ImGui::SliderInt("백 버퍼 개수", &s.backBuffers, 2, 3);
    Help("Swap Chain: 화면에 보여줄 그림판 여러 장을 돌려 쓰는 장치.\n"
         "2장 = 한 장은 모니터가 보여주는 중, 한 장에 그리는 중. 그리기가 끝나도 교체를 기다려야 할 때가 많다.\n"
         "3장 = 여유 그림판이 하나 더 있어서 GPU가 덜 기다린다. 대신 화면 크기만큼 VRAM을 한 장 더 쓴다.");
    CodeVar("s.backBuffers", CV(s.backBuffers), "[시나리오 8] ResizeSwapChain()");
    ImGui::Checkbox("VSync (모니터 주사율에 맞추기)", &s.vsync);
    Help("VSync 켬: 모니터가 그림을 바꾸는 순간(예: 60Hz면 16.7ms마다)까지 Present가 기다린다. 찢어짐은 없지만 FPS가 묶인다.\n"
         "비교 실험을 할 때는 꺼 두어야 차이가 숫자로 보인다.");
    CodeVar("s.vsync", CV(s.vsync), "[시나리오 8] SubmitAndPresent(): Present(1 또는 0)");
    ImGui::Text("  백 버퍼 1장 = %s (VRAM)", MB((uint64_t)g_width * g_height * 4).c_str());
}

// 9. VRAM 압박
static void PanelScenario9_Pressure(Settings& s)
{
    if (!ImGui::CollapsingHeader("9. VRAM 압박 실험 (예산 넘기기)")) return;
    int maxMB = PressureMaxMB(s.pressureHeap);
    ImGui::SliderInt("추가 할당 (MB)", &s.pressureMB, 0, maxMB, "%d MB", ImGuiSliderFlags_AlwaysClamp);
    Help("128MB짜리 버퍼를 계속 만들어 메모리를 채운다. 값을 놓는 순간 적용.\n"
         "VRAM 예산을 넘기면 OS(VidMm)가 일부를 시스템 RAM으로 내보낸다(Demotion). 그러면 GPU가 느린 길로 읽게 된다.\n"
         "안전을 위해 최대값은 VRAM 예산 + 1GB (Upload 힙은 시스템 RAM 3GB를 남기는 만큼)로 제한했다.");
    CodeVar("s.pressureMB", CV(s.pressureMB), "[시나리오 9] ApplyPressure()");
    ImGui::RadioButton("Default 힙 (VRAM)##pressure", &s.pressureHeap, 0); ImGui::SameLine();
    ImGui::RadioButton("Upload 힙 (시스템 RAM)##pressure", &s.pressureHeap, 1);
    Help("Upload 힙으로 바꾸면 VRAM 막대는 그대로이고 공유 GPU 메모리와 이 프로그램 RAM이 오른다.");
    CodeVar("s.pressureHeap", s.pressureHeap ? "1 (Upload)" : "0 (Default)", "[시나리오 9] ApplyPressure()");
    ImGui::Checkbox("매 프레임 전부 사용하기", &s.pressureTouch);
    Help("할당만 한 메모리는 OS가 몰래 밖으로 빼 둘 수 있다. 매 프레임 조금씩이라도 쓰면 전부 VRAM에 있어야 하므로\n"
         "예산을 넘긴 상태에서는 매 프레임 메모리를 넣었다 뺐다(페이징) 하며 심하게 끊긴다.\n"
         "안전을 위해 한 프레임에는 최대 1GB(8덩어리)만 돌아가며 사용한다. 그래도 예산을 넘기면 매 프레임 페이징이 일어난다.");
    CodeVar("s.pressureTouch", CV(s.pressureTouch), "[시나리오 9] TouchPressureChunks()");
    ImGui::BeginDisabled(s.pressureTouch);
    ImGui::Checkbox("안 쓰는 동안 Evict 해 두기", &s.pressureEvict);
    ImGui::EndDisabled();
    Help("Evict: '이건 당분간 안 쓸 테니 VRAM에서 빼도 돼'라고 OS에 알리는 호출. 다시 쓰기 전엔 MakeResident 해야 한다.\n"
         "쉽게 말하면 안 입는 겨울옷을 창고로 보내 옷장을 비우는 것.");
    CodeVar("s.pressureEvict", CV(s.pressureEvict), "[시나리오 9] SetPressureResidency(): Evict / MakeResident");
    ImGui::Text("  현재 %d MB 할당 %s", (int)(g_pressure.size() * 128), g_pressureEvicted ? "(Evict 됨)" : "");
    if (!g_pressureError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "  %s", g_pressureError.c_str());
}

// 10. 텍스처
static void PanelScenario10_Texture(Settings& s)
{
    if (!ImGui::CollapsingHeader("10. 텍스처 (4K 텍스처 실험)")) return;
    ImGui::TextUnformatted("텍스처 크기:");
    Help("행성과 큐브에 씌운 체크무늬 텍스처의 크기. 파일 없이 코드로 만든다 (RGBA 8비트 = 픽셀당 4바이트).\n"
         "256 = 256KB, 1K = 4MB, 4K(4096x4096) = 64MB. 4K는 256의 256배다.\n"
         "바꾸는 순간 GPU를 멈추고 새로 만들어 복사 큐로 올린다 (아래 '처음 만들 때' 시간).\n"
         "쉽게 말하면 같은 그림을 엽서로 뽑느냐, 벽화 크기로 뽑느냐.");
    ImGui::RadioButton("256", &s.texSize, 0); ImGui::SameLine();
    ImGui::RadioButton("1K (1024)", &s.texSize, 1); ImGui::SameLine();
    ImGui::RadioButton("4K (4096)", &s.texSize, 2);
    CodeVar("s.texSize", s.texSize == 0 ? "0 (256)" : s.texSize == 1 ? "1 (1024)" : "2 (4096)", "[시나리오 10] RebuildTexture()");
    ImGui::Checkbox("밉맵 만들기", &s.texMips);
    Help("[쉽게 말하면] 엄지손톱만 한 그림을 그리려고 벽화 원본을 돋보기로 훑는 것 vs 미리 축소해 둔 엽서를 보는 것.\n\n"
         "[뜻] 밉맵(Mipmap) = 원본을 가로세로 절반씩 줄인 그림을 1x1까지 미리 만들어 같이 넣어 둔 것 (4096, 2048, 1024 ... 1).\n"
         "GPU는 화면에 작게 보이는 물체일수록 작은 단계를 골라 읽는다 (샘플러의 MIP 필터).\n\n"
         "[밉맵이 없으면] 화면에서 몇십 픽셀인 큐브도 4K 원본을 읽는다. 픽셀마다 텍스처의 멀리 떨어진 곳을 듬성듬성 읽어서\n"
         "GPU 캐시가 계속 빗나가고(느림), 멀리 있는 무늬가 반짝거린다(지글거림).\n\n"
         "[대가] 메모리가 약 1/3 더 든다 (4K: 64MB -> 약 85MB).\n"
         "볼 숫자: 'GPU: 큐브 ms', 'GPU: 행성 ms', 그리고 아래 텍스처 크기. 차이 크기는 GPU마다 다르다.");
    CodeVar("s.texMips", CV(s.texMips), "[시나리오 10] GenerateTexturePixels()");
    ImGui::BeginDisabled(!s.texMips);
    ImGui::Checkbox("밉 단계마다 색 칠하기 (눈으로 확인용)", &s.texMipTint);
    ImGui::EndDisabled();
    Help("밉 단계마다 색을 입힌다: 원본 = 회색, 1단계 = 초록, 2단계 = 파랑, 3단계 = 노랑, 4단계 = 빨강 ...\n"
         "가까운 면은 회색, 멀어질수록 초록 -> 파랑 -> 노랑으로 바뀌는 게 보이면 'GPU가 작은 판을 골라 읽는다'는 증거다.\n"
         "4K로 두면 큐브는 대부분 노랑/빨강(아주 작은 판)으로 보인다 = 4K 원본은 거의 안 읽힌다.");
    CodeVar("s.texMipTint", CV(s.texMipTint), "[시나리오 10] GenerateTexturePixels()");
    ImGui::Checkbox("매 프레임 텍스처 새로 만들고 올리기", &s.texRecreate);
    Help("매 프레임 CreateCommittedResource로 텍스처를 새로 만들고, CPU가 픽셀 전체를 Upload 힙에 다시 쓴 뒤\n"
         "그리기 직전에 CopyTextureRegion으로 VRAM에 복사하고, 배리어로 '셰이더가 읽는 용도'로 바꾼다.\n"
         "게임에서 새 지역에 들어가거나 텍스처를 그때그때 불러올 때 '툭' 끊기는 현상을 매 프레임 일으키는 것.\n"
         "볼 숫자: 'CPU 작업 ms'(64MB memcpy), 'GPU 전체 ms'(복사 시간 포함), 오른쪽 위 그래프, 메모리 표의 Upload Heap.\n"
         "정석: 로딩 때 복사 큐로 한 번 올려 두고 계속 쓴다 (또는 스트리밍으로 조금씩, 미리).");
    CodeVar("s.texRecreate", CV(s.texRecreate), "[시나리오 10] CreateTextureForFrame() + RecordTextureRecreate()");
    Verdict(s.texMips && !s.texRecreate, "밉맵 있음 + 한 번 올려 두고 재사용",
            s.texRecreate ? "매 프레임 텍스처 생성 + 전체 복사 (끊김의 원인)" : "밉맵 없음: 작게 보여도 원본을 읽음");
    uint64_t texels = 0;
    for (auto& m : g_texCpu) texels += m.size();
    UINT sz = g_texBuiltSize >= 0 ? kTexSizes[g_texBuiltSize] : 0;
    ImGui::Text("  텍스처 1장 (VRAM): %s · %ux%u · 밉 %u단계", MB(g_texAllocBytes).c_str(), sz, sz, g_texMipCount);
    ImGui::Text("  처음 만들 때: 픽셀 계산 %.1f ms + 복사 큐 업로드 %.1f ms", g_texGenMs, g_texUploadMs);
    ImGui::Text("  같은 텍스처를 압축하면: BC7 %s · BC1 %s", MB(texels).c_str(), MB(texels / 2).c_str());
    Help("BC7, BC1 = GPU가 압축된 채로 바로 읽는 텍스처 포맷 (Block Compression). 4x4 픽셀을 한 덩어리로 줄여 저장한다.\n"
         "RGBA 8비트(픽셀당 4바이트)에 비해 BC7은 1바이트(1/4), BC1은 0.5바이트(1/8).\n"
         "VRAM도 줄고, GPU가 읽는 양도 줄어서 빨라진다. 그래서 게임 텍스처는 거의 다 압축 포맷이다.\n"
         "(이 데모는 실행 중에 압축하지 않고, 같은 크기일 때의 숫자만 비교해 보여 준다)");
    if (g_stats.texBytesThisFrame)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "  이번 프레임 새로 만든 텍스처: VRAM %s + Upload %s", MB(g_stats.texBytesThisFrame).c_str(), MB(g_stats.texBytesThisFrame).c_str());
}

static void DrawPanel(Settings& s, float panelX, float panelW)
{
    ImGui::SetNextWindowPos(ImVec2(panelX, 0));
    ImGui::SetNextWindowSize(ImVec2(panelW, (float)g_height));
    ImGui::Begin("DX12 메모리 최적화 데모", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    PanelPresets(s);
    PanelPerformance();
    PanelMemory();
    PanelScenario1_MeshHeap(s);
    PanelScenario2_Buffers(s);
    PanelScenario3_Barriers(s);
    PanelScenario4_Sync(s);
    PanelScenario5_DrawCalls(s);
    PanelScenario6_State(s);
    PanelScenario7_Readback(s);
    PanelScenario8_SwapChain(s);
    PanelScenario9_Pressure(s);
    PanelScenario10_Texture(s);
    ImGui::End();
}

// ---------------------------------------------------------------------
// 프레임
// ---------------------------------------------------------------------
static Settings g_applied;   // 마지막으로 적용된(재생성이 필요한) 값들
static int  g_uiMeshTrisK = 600, g_uiPressureMB = 0;

static void SetCommonState(Stats& st, ID3D12GraphicsCommandList* cl, FrameRes& fr, D3D12_GPU_VIRTUAL_ADDRESS instVA,
                           const D3D12_VIEWPORT& vp, const D3D12_RECT& sc, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    cl->SetGraphicsRootSignature(g_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootConstantBufferView(1, fr.frameCB->VA());
    cl->SetGraphicsRootShaderResourceView(2, instVA);
    cl->SetGraphicsRootConstantBufferView(3, fr.frameCB->VA());
    cl->SetGraphicsRootDescriptorTable(4, fr.texBind);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    st.rootSigSets++;
}

static void ReadSceneColor(FrameRes& fr)
{
    if (!fr.sceneReadback) return;
    const uint8_t* p = fr.sceneReadback->mapped + g_rbFootprint.Offset;
    double r = 0, g = 0, b = 0; int n = 0;
    for (UINT y = 0; y < g_rbFootprint.Footprint.Height; y += 16)
        for (UINT x = 0; x < g_rbFootprint.Footprint.Width; x += 16)
        {
            const uint8_t* px = p + (uint64_t)y * g_rbFootprint.Footprint.RowPitch + x * 4;
            r += px[0]; g += px[1]; b += px[2]; n++;
        }
    if (n) { g_rbColor[0] = (float)(r / n / 255); g_rbColor[1] = (float)(g / n / 255); g_rbColor[2] = (float)(b / n / 255); }
}

// 개발용 자동 테스트: 환경변수 DEMO_AUTOTEST=1 이면 모든 조합을 차례로 돌려 보고 종료
static bool g_autotest = false;
static void AutotestStep(Settings& s)
{
    int step = (int)(g_frameNo / 4);
    Settings b; PresetBest(b); b.meshTrisK = 40; b.objectCount = 300; b.meshRepeat = 1;
    Settings t = b;
    switch (step)
    {
    case 0: break;
    case 1: t.meshHeap = MH_UPLOAD; break;
    case 2: t.meshHeap = MH_UPLOAD; t.meshReupload = true; break;
    case 3: t.meshHeap = MH_DEFAULT; t.meshReupload = true; break;
    case 4: t.meshHeap = MH_GPU_UPLOAD; break;
    case 5: t.bufferMode = BM_NEW_BIG; break;
    case 6: t.bufferMode = BM_NEW_PER_OBJECT; break;
    case 7: t.uselessBarriers = 50; break;
    case 8: t.uselessBarriers = 50; t.instancing = false; break;
    case 9: t.framesInFlight = 1; break;
    case 10: t.framesInFlight = 3; break;
    case 11: t.instancing = false; t.resetStatePerDraw = true; break;
    case 12: t.recompilePso = true; break;
    case 13: t.readback = RB_WAIT_NOW; break;
    case 14: t.readback = RB_DELAYED; break;
    case 15: t.backBuffers = 2; break;
    case 16: t.vsync = true; break;
    case 17: t.pressureMB = 512; break;
    case 18: t.pressureMB = 512; t.pressureTouch = true; break;
    case 19: t.pressureMB = 512; t.pressureEvict = true; break;
    case 20: t.pressureMB = 256; t.pressureHeap = 1; break;
    case 21: PresetWorst(t); t.meshTrisK = 40; t.objectCount = 300; break;
    case 22: t.objectCount = 0; t.meshTrisK = 0; t.uselessBarriers = 5; break;
    case 23: g_wireframe = true; break;
    case 24: g_wireframe = true; t.instancing = false; t.resetStatePerDraw = true; t.recompilePso = true; break;
    case 25: t.cpuLogicMs = 5; t.framesInFlight = 1; break;
    case 26: case 27: case 28: break;   // UI에서 프레임 중간에 설정이 바뀌는 경우 (DrawPanelAndApply에서 흔듦)
    case 29: t.texSize = 0; break;
    case 30: t.texSize = 1; t.texMips = false; break;
    case 31: t.texSize = 1; t.texMipTint = true; break;
    case 32: t.texRecreate = true; break;
    case 33: t.texSize = 0; t.texRecreate = true; t.framesInFlight = 3; break;
    case 34: PresetWorst(t); t.meshTrisK = 40; t.objectCount = 300; t.texSize = 1; break;
    default: g_wireframe = false; PostQuitMessage(0); break;
    }
    if (t.meshHeap == MH_GPU_UPLOAD && !g_gpuUploadSupported) t.meshHeap = MH_DEFAULT;
    s = t;
    g_uiMeshTrisK = s.meshTrisK; g_uiPressureMB = s.pressureMB;
    if (g_frameNo % 4 == 0) { printf("autotest step %d ok (frame %llu)\n", step, (unsigned long long)g_frameNo); fflush(stdout); }
}

// 자동 테스트: 프레임마다 숫자(드로우콜, 배리어, 힙별 바이트 ...)를 남긴다. 코드를 고친 전후로 비교하면 동작이 같은지 알 수 있다.
static void AutotestLogStats()
{
    const Stats& st = g_stats;
    printf("STATS f=%llu dc=%d bar=%d pso=%d rs=%d cr=%d uv=%llu sv=%llu ut=%llu st=%llu mr=%llu mc=%llu tx=%llu hb=%lld,%lld,%lld,%lld hc=%d,%d,%d,%d rb=%d\n",
           (unsigned long long)g_frameNo, st.drawCalls, st.barriers, st.psoSets, st.rootSigSets, st.created,
           (unsigned long long)st.uniqueVerts, (unsigned long long)st.submittedVerts, (unsigned long long)st.uniqueTris, (unsigned long long)st.submittedTris,
           (unsigned long long)st.meshBytesReadPerFrame, (unsigned long long)st.meshBytesCopiedPerFrame, (unsigned long long)st.texBytesThisFrame,
           (long long)g_heapBytes[0], (long long)g_heapBytes[1], (long long)g_heapBytes[2], (long long)g_heapBytes[3],
           g_heapCount[0], g_heapCount[1], g_heapCount[2], g_heapCount[3], g_rbAgeFrames);
}

// =====================================================================
//  한 프레임 (DirectX-Graphics-Samples 의 OnUpdate / OnRender / PopulateCommandList 처럼 단계별 함수로 나눔)
//
//  RenderFrame()
//    1) BeginFrameTiming        프레임 시간 재기, 설정이 바뀌면 평균 다시 모으기
//    2) ApplyPendingRebuilds    GPU를 멈추고 다시 만들어야 하는 설정 적용 (메시, 텍스처, 스왑체인, VRAM 압박)
//    3) WaitForFrameSlot        [시나리오 4] 펜스 대기
//    4) ReadFinishedFrameResults  이 프레임 세트가 지난번에 남긴 결과 읽기 (GPU 시간, 통계, 되읽기)
//    5) BuildUi                 ImGui 패널, 큰 글씨, 그래프
//    6) PrepareCpuData          상수 버퍼, 게임 로직 흉내, 큐브 위치, 메시/PSO/텍스처 준비
//    7) PopulateCommandList     GPU 명령 기록 (행성, 큐브, 배리어, 되읽기 복사, UI)
//    8) SubmitAndPresent        제출 + Present
//    9) WaitReadbackNow         [시나리오 7] 최악: 바로 기다리기
//   10) UpdateStatsAndHistory   통계와 그래프 기록
// =====================================================================

// 한 프레임 동안 단계들이 같이 쓰는 값 (예전 RenderFrame 의 지역 변수들)
struct FrameCtx
{
    Settings* s = nullptr;            // = g_s (패널에서 바꾸는 지금 설정)
    double tStart = 0;                // 프레임 시작 시각
    UINT fi = 0;                      // 이번 프레임이 쓰는 프레임 자원 세트 번호
    FrameRes* fr = nullptr;           // 그 세트
    double waitMs = 0;                // 펜스로 기다린 시간
    double cpuWorkStart = 0, cpuWorkMs = 0, presentMs = 0;
    float sceneW = 1;                 // 패널을 뺀 렌더 화면 폭
    float time = 0;                   // 애니메이션용 시간(초)
    Stats st{};                       // 이번 프레임 통계
    int count = 0;                    // 이번 프레임 큐브 수
    D3D12_GPU_VIRTUAL_ADDRESS instVA = 0;               // 큐브 위치 버퍼 주소
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> objCB;       // "물체마다 버퍼" 모드의 큐브별 버퍼 주소
    ID3D12Resource* meshVB = nullptr; uint64_t meshBytes = 0; bool meshOk = false;   // 행성
    ID3D12PipelineState* psoA = nullptr; ID3D12PipelineState* psoB = nullptr;
    ResPtr texNew;                    // [시나리오 10] 매 프레임 새로 만든 텍스처
    ID3D12GraphicsCommandList* cl = nullptr;
    UINT tsBase = 0;                  // 타임스탬프 쿼리 시작 번호
    ID3D12Resource* back = nullptr;   // 이번 백 버퍼
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
    D3D12_VIEWPORT vp{}; D3D12_RECT sc{};
};

// ---------------------------------------------------------------------
// 1) ~ 4) 프레임 시작
// ---------------------------------------------------------------------
static void BeginFrameTiming(FrameCtx& f)
{
    static double lastFrameStart = NowMs();
    f.tStart = NowMs();
    g_stats.frameMs = f.tStart - lastFrameStart;
    lastFrameStart = f.tStart;
    Settings& s = *f.s;
    if (g_autotest) AutotestStep(s);
    static Settings prevSettings = s;
    if (!SameSettings(prevSettings, s)) { ResetAverage(f.tStart); prevSettings = s; }   // 설정이 바뀌면 평균을 새로 모음
}

// 재생성이 필요한 변경 적용 (GPU를 멈추고)
static void ApplyPendingRebuilds(Settings& s)
{
    if (g_resizePending) { g_resizePending = false; if (g_newW && g_newH) ResizeSwapChain(g_newW, g_newH, g_bbCount); }
    if ((UINT)s.backBuffers != g_bbCount) ResizeSwapChain(g_width, g_height, (UINT)s.backBuffers);
    if (s.meshTrisK != g_meshBuiltTrisK || s.meshHeap != g_meshBuiltHeap || s.meshReupload != g_meshBuiltReupload) RebuildMesh(s);
    s.pressureMB = std::min(s.pressureMB, PressureMaxMB(s.pressureHeap));
    if (s.pressureMB != g_applied.pressureMB || s.pressureHeap != g_applied.pressureHeap)
    {
        ApplyPressure(s);
        g_applied.pressureMB = s.pressureMB; g_applied.pressureHeap = s.pressureHeap;
    }
    if (s.texSize != g_texBuiltSize || (int)s.texMips != g_texBuiltMips || (int)s.texMipTint != g_texBuiltTint) RebuildTexture(s);
    bool wantEvict = s.pressureEvict && !s.pressureTouch;
    if (wantEvict != g_pressureEvicted) SetPressureResidency(wantEvict);
}

// 프레임 자원 확보: 동시에 진행할 프레임 수만큼만 앞서 가도록 펜스 대기
static void WaitForFrameSlot(FrameCtx& f)
{
    double tw0 = NowMs();
    f.fi = (UINT)(g_frameNo % kMaxFrames);
    f.fr = &g_frames[f.fi];
    // ===== [시나리오 4] 펜스 대기: "동시에 진행할 프레임 수"만큼만 앞서 가게 막는 곳 =====
    if (g_frameNo >= (uint64_t)f.s->framesInFlight)
        WaitFence(g_fence.Get(), g_frameFence[(g_frameNo - f.s->framesInFlight) % 8]);
    if (g_frameNo >= kMaxFrames)
        WaitFence(g_fence.Get(), g_frameFence[(g_frameNo - kMaxFrames) % 8]);
    f.waitMs = NowMs() - tw0;
}

// 이 세트를 마지막으로 쓴 프레임의 결과 읽기 (GPU가 이미 끝냄)
static void ReadFinishedFrameResults(FrameCtx& f)
{
    FrameRes& fr = *f.fr;
    if (fr.tsValid)
    {
        const uint64_t* ts = (const uint64_t*)fr.tsReadback->mapped;
        double k = 1000.0 / (double)g_tsFreq;
        if (ts[3] > ts[0]) { g_stats.gpuTotalMs = (ts[3] - ts[0]) * k; g_stats.gpuMeshMs = (ts[1] - ts[0]) * k; g_stats.gpuCubesMs = (ts[2] - ts[1]) * k; }
    }
    if (fr.statsValid)
    {
        const D3D12_QUERY_DATA_PIPELINE_STATISTICS* ps = (const D3D12_QUERY_DATA_PIPELINE_STATISTICS*)fr.statsReadback->mapped;
        g_stats.gpuStatsValid = true;
        g_stats.gpuIAVerts = ps->IAVertices; g_stats.gpuVSInvocations = ps->VSInvocations;
        g_stats.gpuIAPrims = ps->IAPrimitives; g_stats.gpuRasterPrims = ps->CPrimitives; g_stats.gpuPSInvocations = ps->PSInvocations;
    }
    // ===== [시나리오 7] 좋은 방법: 몇 프레임 뒤, 이미 끝난 결과를 읽기 =====
    if (fr.rbValid && f.s->readback == RB_DELAYED) { ReadSceneColor(fr); g_rbAgeFrames = (int)(g_frameNo - fr.frameNo); }
    fr.trash.clear();       // 예전에 만든 리소스는 이제 지워도 안전
    fr.trashPso.clear();
    fr.frameNo = g_frameNo;
    fr.texBind = g_texGpu;  // 기본: 한 번 만들어 둔 텍스처
}

// ---------------------------------------------------------------------
// 5) UI
// ---------------------------------------------------------------------
static float s_panelW = 0.0f;   // 패널 폭: 처음엔 자동, 왼쪽 경계를 드래그하면 바뀜 (더블클릭 = 기본값)

static void HandleHotkeys(Settings& s)
{
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) PresetWorst(s);
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) PresetBest(s);
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) g_panelHidden = !g_panelHidden;
    if (ImGui::IsKeyPressed(ImGuiKey_F4)) { g_wireframe = !g_wireframe; ResetAverage(NowMs()); }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) ResetAverage(NowMs());
}

// 패널 왼쪽 경계의 손잡이: 좌우로 드래그해서 폭 조절
static void DrawPanelSplitter(float sceneW, float defaultPanelW, float scale)
{
    const float grip = 10.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(sceneW - grip * 0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(grip, (float)g_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    ImGui::Begin("##panel_splitter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::InvisibleButton("##grip", ImVec2(grip, (float)g_height));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (act) s_panelW -= ImGui::GetIO().MouseDelta.x;
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) s_panelW = defaultPanelW;
    ImU32 col = act ? IM_COL32(120, 200, 255, 255) : hov ? IM_COL32(120, 200, 255, 160) : IM_COL32(140, 165, 200, 170);
    float cx = sceneW;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - 1.5f, 0), ImVec2(cx + 1.5f, (float)g_height), col);
    for (int k = -1; k <= 1; k++)   // 가운데 손잡이 점 3개
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cx, g_height * 0.5f + k * 9.0f * scale), 2.5f * scale, col);
    if (hov && !act) ImGui::SetTooltip("드래그: 패널 폭 조절 / 더블클릭: 기본 폭");
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// 패널을 그리고, 패널에서 바꾼 값을 설정에 반영한다
static void DrawPanelAndApply(Settings& s, float sceneW, float panelW, float defaultPanelW, float scale)
{
    // 슬라이더는 '놓을 때' 적용 (드래그 중에 메시를 수십 번 다시 만들지 않도록)
    Settings ui = s;
    if (g_autotest) { g_uiMeshTrisK = s.meshTrisK; g_uiPressureMB = s.pressureMB; }
    ui.meshTrisK = g_uiMeshTrisK; ui.pressureMB = g_uiPressureMB;
    DrawPanel(ui, sceneW, panelW);
    DrawPanelSplitter(sceneW, defaultPanelW, scale);
    g_uiMeshTrisK = ui.meshTrisK; g_uiPressureMB = ui.pressureMB;
    if (!ImGui::IsAnyItemActive()) { ui.meshTrisK = g_uiMeshTrisK; ui.pressureMB = g_uiPressureMB; }
    else { ui.meshTrisK = s.meshTrisK; ui.pressureMB = s.pressureMB; }
    s = ui;
    if (g_autotest)
    {
        int step = (int)(g_frameNo / 4);
        // 렌더 직전에 설정이 바뀌는 상황 재현 (예전에 이 경우 앱이 종료됨)
        if (step == 26) { s.meshHeap = MH_DEFAULT; s.meshReupload = (g_frameNo & 1) != 0; }
        if (step == 27) { s.meshReupload = true; s.meshHeap = (g_frameNo & 1) ? MH_DEFAULT : MH_UPLOAD; }
        if (step == 28) { s.meshReupload = (g_frameNo & 1) != 0; s.meshHeap = (g_frameNo & 2) ? MH_DEFAULT : MH_UPLOAD; }
    }
}

// 화면 위 큰 글씨 (청중용). 그래프가 피해 갈 수 있게 글씨가 차지한 오른쪽 끝/아래 끝을 돌려준다.
static void DrawBigText(float& bigTextRight, float& bigTextBottom)
{
    char buf[128];
    snprintf(buf, sizeof buf, "%.0f FPS   프레임 %.1f ms   GPU %.1f ms  (%.0f초 평균)", g_avg.fps, g_avg.frame, g_avg.gpu, (double)g_avgSeconds);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float fs = ImGui::GetFontSize() * 2.0f;
    dl->AddText(ImGui::GetFont(), fs, ImVec2(22, 18), IM_COL32(0, 0, 0, 200), buf);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(20, 16), IM_COL32(255, 255, 255, 255), buf);
    char vb[160];
    snprintf(vb, sizeof vb, "VRAM %s / %s    공유 %s    시스템 RAM %s / %s",
             MB(g_mem.local.CurrentUsage).c_str(), MB(g_mem.local.Budget).c_str(), MB(g_mem.nonLocal.CurrentUsage).c_str(),
             GB(g_mem.sysTotal - g_mem.sysAvail).c_str(), GB(g_mem.sysTotal).c_str());
    dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2(21, 17 + fs * 1.1f), IM_COL32(0, 0, 0, 200), vb);
    dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2(20, 16 + fs * 1.1f), IM_COL32(200, 230, 255, 255), vb);
    char cb2[200];
    snprintf(cb2, sizeof cb2, "정점 %s  (이번 프레임 보낸 정점 %s)    삼각형 %s    드로우콜 %d",
             Num(g_stats.uniqueVerts).c_str(), Num(g_stats.submittedVerts).c_str(), Num(g_stats.uniqueTris).c_str(), g_stats.drawCalls);
    dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2(21, 17 + fs * 1.1f + fs * 0.75f), IM_COL32(0, 0, 0, 200), cb2);
    dl->AddText(ImGui::GetFont(), fs * 0.6f, ImVec2(20, 16 + fs * 1.1f + fs * 0.75f), IM_COL32(255, 225, 160, 255), cb2);
    ImFont* fnt = ImGui::GetFont();
    bigTextRight = 20 + std::max({ fnt->CalcTextSizeA(fs, FLT_MAX, 0, buf).x, fnt->CalcTextSizeA(fs * 0.6f, FLT_MAX, 0, vb).x,
                                   fnt->CalcTextSizeA(fs * 0.6f, FLT_MAX, 0, cb2).x });
    bigTextBottom = 16 + fs * 1.1f + fs * 0.75f + fs * 0.6f;
}

// 그래프 하나 + 60 FPS 점선
static void DrawOverlayGraph(const char* id, const float* h, const char* name, float now, ImU32 lineCol, float top, float gw, float gh)
{
    char lbl[96]; snprintf(lbl, sizeof lbl, "%s  %.2f ms", name, now);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, lineCol);
    ImGui::PlotLines(id, h, kHist, g_histPos, lbl, 0.0f, top, ImVec2(gw, gh));
    ImGui::PopStyleColor();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float pad = ImGui::GetStyle().FramePadding.y;
    float y = b.y - pad - (b.y - a.y - 2 * pad) * (16.7f / top);   // 60 FPS 기준선
    ImDrawList* d = ImGui::GetWindowDrawList();
    for (float x = a.x; x < b.x; x += 8.0f) d->AddLine(ImVec2(x, y), ImVec2(std::min(x + 4.0f, b.x), y), IM_COL32(255, 255, 255, 90));
}

// 렌더 화면 오른쪽 위 그래프 (패널을 안 보고도 설정 효과를 바로 확인)
static void DrawGraphOverlay(float sceneW, float bigTextRight, float bigTextBottom)
{
    float fsz = ImGui::GetFontSize();
    float gw = std::min(fsz * 22.0f, sceneW * 0.45f), gh = fsz * 3.2f;
    float top = 0.0f;   // 두 그래프가 같은 세로 범위를 써야 CPU와 GPU를 눈으로 비교할 수 있다
    for (int i = 0; i < kHist; i++) top = std::max(top, std::max(g_histFrame[i], g_histGpu[i]));
    top = std::max(20.0f, top * 1.15f);
    // 왼쪽 위 큰 글씨와 겹치면 그 아래로 내린다 (화면이 좁을 때)
    float ovY = (sceneW - 12.0f - gw - ImGui::GetStyle().WindowPadding.x * 2 < bigTextRight + 16.0f) ? bigTextBottom + 12.0f : 12.0f;
    ImGui::SetNextWindowPos(ImVec2(sceneW - 12.0f, ovY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##graphOverlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    int last = (g_histPos + kHist - 1) % kHist;
    DrawOverlayGraph("##ovFrame", g_histFrame, "CPU 프레임", g_histFrame[last], IM_COL32(120, 200, 255, 255), top, gw, gh);
    DrawOverlayGraph("##ovGpu", g_histGpu, "GPU", g_histGpu[last], IM_COL32(255, 170, 90, 255), top, gw, gh);
    ImGui::TextDisabled("세로 0 ~ %.0f ms · 점선 = 16.7 ms (60 FPS)", top);
    ImGui::End();
}

static void BuildUi(FrameCtx& f)
{
    f.cpuWorkStart = NowMs();   // 여기부터 명령 목록을 닫을 때까지가 "CPU 작업" 시간
    g_resourcesCreatedThisFrame = 0;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    HandleHotkeys(*f.s);
    float scale = ImGui::GetStyle().FontScaleDpi;
    float defaultPanelW = std::min((float)g_width * 0.5f, std::max(500.0f * scale, (float)g_width * 0.32f));
    if (s_panelW <= 0.0f) s_panelW = defaultPanelW;
    s_panelW = std::clamp(s_panelW, std::min(320.0f * scale, (float)g_width * 0.9f), (float)g_width * 0.9f);
    float panelW = g_panelHidden ? 0.0f : s_panelW;
    f.sceneW = std::max(1.0f, (float)g_width - panelW);
    if (!g_panelHidden) DrawPanelAndApply(*f.s, f.sceneW, panelW, defaultPanelW, scale);
    float bigTextRight = 0.0f, bigTextBottom = 0.0f;   // 그래프가 큰 글씨를 피해 가도록
    DrawBigText(bigTextRight, bigTextBottom);
    if (g_showGraphOverlay) DrawGraphOverlay(f.sceneW, bigTextRight, bigTextBottom);
    ImGui::Render();
}

// ---------------------------------------------------------------------
// 6) CPU 데이터 준비
// ---------------------------------------------------------------------
// 카메라 + 조명 상수를 이번 프레임 상수 버퍼(Upload 힙)에 쓴다
static void WriteFrameConstants(FrameCtx& f)
{
    FrameCB* cb = (FrameCB*)f.fr->frameCB->mapped;
    float camA = f.time * 0.05f;
    Vec3 eye{ sinf(camA) * 15.0f, 5.5f, cosf(camA) * 15.0f };
    Mat4 v = LookAtLH(eye, { 0, 0, 0 }, { 0, 1, 0 });
    Mat4 p = PerspectiveLH(55.0f * 3.14159f / 180.0f, f.sceneW / (float)g_height, 0.1f, 100.0f);
    FrameCB c{};
    c.viewProj = Mul(p, v);
    c.time = f.time; c.meshScale = 3.0f;
    Vec3 l = Normalize({ -0.4f, -1.0f, 0.3f });
    c.lightDir[0] = l.x; c.lightDir[1] = l.y; c.lightDir[2] = l.z;
    memcpy(cb, &c, sizeof c);
}

// ===== [시나리오 4] 게임 로직 흉내: CPU를 일부러 바쁘게 =====
static void SimulateGameLogic(const Settings& s)
{
    if (s.cpuLogicMs <= 0) return;
    double until = NowMs() + s.cpuLogicMs;
    volatile double sink = 0;
    while (NowMs() < until) sink = sink + 1.0;
}

// 큐브 위치 계산 (행성 주위 여러 겹의 궤도). 결과는 CPU 메모리(std::vector)
static std::vector<Instance>& ComputeCubePositions(int count, float t)
{
    static std::vector<Instance> inst(kMaxObjects);
    for (int i = 0; i < count; i++)
    {
        float fi2 = (float)i;
        float shell = 4.2f + (float)(i % 7) * 0.9f;
        float y = 1.0f - 2.0f * (fi2 + 0.5f) / (float)std::max(count, 1);
        float r = sqrtf(std::max(0.0f, 1.0f - y * y));
        float a = fi2 * 2.39996f + t * (0.6f / shell);
        Instance& d = inst[i];
        d.px = cosf(a) * r * shell; d.py = y * shell * 0.75f; d.pz = sinf(a) * r * shell;
        d.scale = 0.16f;
        d.r = 0.5f + 0.5f * sinf(fi2 * 0.37f); d.g = 0.5f + 0.5f * sinf(fi2 * 0.11f + 2.0f); d.b = 0.5f + 0.5f * sinf(fi2 * 0.23f + 4.0f);
        d.phase = fmodf(fi2 * 0.618f, 1.0f);
    }
    return inst;
}

// ===== [시나리오 2] 최적: 미리 만든 버퍼 재사용 / 최악: 매 프레임 새로 만들기 =====
static void UploadCubeInstances(FrameCtx& f, const std::vector<Instance>& inst)
{
    const Settings& s = *f.s;
    FrameRes& fr = *f.fr;
    int count = f.count;
    f.instVA = fr.inst->VA();
    if (s.bufferMode == BM_REUSE)
        memcpy(fr.inst->mapped, inst.data(), sizeof(Instance) * count);
    else if (s.bufferMode == BM_NEW_BIG)
    {
        ResPtr b = MustBuffer(HK_UPLOAD, sizeof(Instance) * std::max(count, 1), true);
        memcpy(b->mapped, inst.data(), sizeof(Instance) * count);
        f.instVA = b->VA();
        fr.trash.push_back(std::move(b));   // GPU가 다 쓴 뒤(이 세트가 다시 올 때) 삭제
    }
    else
    {
        f.objCB.resize(count);
        for (int i = 0; i < count; i++)
        {
            ResPtr b = MustBuffer(HK_UPLOAD, 256, true);  // 32바이트 데이터지만 64KB가 잡힌다
            memcpy(b->mapped, &inst[i], sizeof(Instance));
            f.objCB[i] = b->VA();
            fr.trash.push_back(std::move(b));
        }
    }
}

// 행성 메시: 매 프레임 재전송이면 이번 프레임 버퍼에 통째로 memcpy
static void PrepareMeshForFrame(FrameCtx& f)
{
    FrameRes& fr = *f.fr;
    f.meshVB = nullptr;
    f.meshBytes = g_meshCpu.size() * sizeof(Vertex);
    f.meshOk = !g_meshCpu.empty() && g_meshError.empty();
    if (!f.meshOk) return;
    // 주의: 이번 프레임 UI에서 설정이 바뀌었을 수 있으니 '실제로 만들어 둔' 상태(g_meshBuilt*)를 기준으로 쓴다.
    //       (설정은 다음 프레임 시작 때 RebuildMesh로 반영됨)
    // ===== [시나리오 1] 최악: 매 프레임 CPU 원본 전체를 다시 memcpy =====
    if (g_meshBuiltReupload && fr.meshFrame)
    {
        memcpy(fr.meshFrame->mapped, g_meshCpu.data(), f.meshBytes);
        f.st.meshBytesCopiedPerFrame = f.meshBytes;
        if (g_meshBuiltHeap == MH_DEFAULT) f.meshVB = g_meshStatic ? g_meshStatic->res.Get() : nullptr;
        else f.meshVB = fr.meshFrame->res.Get();
    }
    else if (g_meshStatic) f.meshVB = g_meshStatic->res.Get();
    if (!f.meshVB) f.meshOk = false;
}

// 이번 프레임에 쓸 PSO 고르기 (+ 최악: 매 프레임 새로 만들기)
static void PreparePsosForFrame(FrameCtx& f)
{
    f.psoA = g_wireframe ? g_psoWireA.Get() : g_psoA.Get();
    f.psoB = g_wireframe ? g_psoWireB.Get() : g_psoB.Get();
    // ===== [시나리오 6] 최악: 매 프레임 셰이더 컴파일 + PSO 생성 =====
    if (f.s->recompilePso)
    {
        ComPtr<ID3DBlob> vs, ps;
        if (CompileShaders(vs, ps))
        {
            ComPtr<ID3D12PipelineState> p = CreatePso(vs.Get(), ps.Get(), 0, g_wireframe);
            f.psoA = p.Get();
            f.fr->trashPso.push_back(p);
        }
    }
}

// ===== [시나리오 10] 최악: 매 프레임 텍스처를 새로 만듦 (CreateCommittedResource로 VRAM 자리부터 다시) =====
static void CreateTextureForFrame(FrameCtx& f)
{
    if (f.s->texRecreate && g_tex)
        f.texNew = CreateTracked(HK_DEFAULT, TextureDesc(), D3D12_RESOURCE_STATE_COPY_DEST, nullptr, false);
}

static void PrepareCpuData(FrameCtx& f)
{
    const Settings& s = *f.s;
    f.time = (float)(NowMs() * 0.001);
    f.count = s.bufferMode == BM_NEW_PER_OBJECT ? std::min(s.objectCount, kMaxPerObjectCB) : s.objectCount;
    WriteFrameConstants(f);
    SimulateGameLogic(s);
    UploadCubeInstances(f, ComputeCubePositions(f.count, f.time));
    PrepareMeshForFrame(f);
    PreparePsosForFrame(f);
    CreateTextureForFrame(f);
}

// ---------------------------------------------------------------------
// 7) 명령 기록 (Samples 의 PopulateCommandList)
// ---------------------------------------------------------------------
// 명령 목록 열기 + 백 버퍼를 "렌더 타깃"으로 + 화면/깊이 지우기
static void BeginCommandList(FrameCtx& f)
{
    FrameRes& fr = *f.fr;
    HR(fr.alloc->Reset());
    HR(g_list->Reset(fr.alloc.Get(), f.psoA));
    f.cl = g_list.Get();
    ID3D12GraphicsCommandList* cl = f.cl;
    f.tsBase = f.fi * kTsPerFrame;
    cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, f.tsBase + 0);
    if (g_pipeStatsHeap) cl->BeginQuery(g_pipeStatsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, f.fi);   // 장면(행성+큐브)만 센다

    UINT bb = g_swapChain->GetCurrentBackBufferIndex();
    f.back = g_backBuffers[bb].Get();
    D3D12_RESOURCE_BARRIER b0 = Transition(f.back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, &b0); f.st.barriers++;

    f.rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart(); f.rtv.ptr += (SIZE_T)bb * g_rtvInc;
    f.dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[4] = { 0.04f, 0.05f, 0.09f, 1 };
    cl->ClearRenderTargetView(f.rtv, clear, 0, nullptr);
    cl->ClearDepthStencilView(f.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    f.vp = D3D12_VIEWPORT{ 0, 0, f.sceneW, (float)g_height, 0, 1 };
    f.sc = D3D12_RECT{ 0, 0, (LONG)f.sceneW, (LONG)g_height };
}

// ===== [시나리오 10] 최악: 픽셀 전체를 Upload 힙에 다시 쓰고, 그리기 직전에 VRAM으로 복사 =====
static void RecordTextureRecreate(FrameCtx& f)
{
    if (!f.texNew) return;
    FrameRes& fr = *f.fr;
    ResPtr staging = StageAndRecordTextureCopy(f.cl, f.texNew->res.Get(), TextureDesc());   // CPU memcpy + CopyTextureRegion
    if (!staging) return;
    D3D12_RESOURCE_BARRIER tb = Transition(f.texNew->res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    f.cl->ResourceBarrier(1, &tb); f.st.barriers++;                  // 복사 끝 -> 셰이더가 읽는 용도로
    WriteTextureSrv(f.texNew->res.Get(), g_texFrameSrvCpu[f.fi]);    // 이 프레임 세트 전용 명함 자리에 새 SRV
    fr.texBind = g_texFrameSrvGpu[f.fi];
    f.st.texBytesThisFrame = f.texNew->alloc;
    fr.trash.push_back(std::move(staging));                          // GPU가 다 쓴 뒤(이 세트가 다시 올 때) 삭제
    fr.trash.push_back(std::move(f.texNew));
}

// 7-1) 행성
static void DrawPlanet(FrameCtx& f)
{
    if (!f.meshOk) return;
    const Settings& s = *f.s;
    ID3D12GraphicsCommandList* cl = f.cl;
    if (g_meshBuiltReupload && g_meshBuiltHeap == MH_DEFAULT && f.fr->meshFrame)
    {
        // Upload(이번 프레임) -> Default 로 복사. 배리어로 용도 전환.
        D3D12_RESOURCE_BARRIER a = Transition(f.meshVB, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->ResourceBarrier(1, &a);
        cl->CopyBufferRegion(f.meshVB, 0, f.fr->meshFrame->res.Get(), 0, f.meshBytes);
        D3D12_RESOURCE_BARRIER b = Transition(f.meshVB, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        cl->ResourceBarrier(1, &b);
        f.st.barriers += 2;
    }
    D3D12_VERTEX_BUFFER_VIEW vbv{ f.meshVB->GetGPUVirtualAddress(), (UINT)f.meshBytes, sizeof(Vertex) };
    cl->IASetVertexBuffers(0, 1, &vbv);
    UINT rc[4] = { 0, 2, 0, 0 };
    cl->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
    // ===== [시나리오 1] 같은 자리에 N번 그리기 (정점 읽기만 N배) =====
    for (int r = 0; r < s.meshRepeat; r++) { cl->DrawInstanced((UINT)g_meshCpu.size(), 1, 0, 0); f.st.drawCalls++; }
    f.st.meshBytesReadPerFrame = f.meshBytes * s.meshRepeat;
    f.st.uniqueVerts += g_meshCpu.size(); f.st.uniqueTris += g_meshCpu.size() / 3;
    f.st.submittedVerts += (uint64_t)g_meshCpu.size() * s.meshRepeat; f.st.submittedTris += (uint64_t)(g_meshCpu.size() / 3) * s.meshRepeat;
}

// ===== [시나리오 3] 쓸데없는 배리어 왕복 (깊이 버퍼: 쓰기 -> 읽기 -> 쓰기) =====
static void RecordUselessBarrier(FrameCtx& f)
{
    // 깊이 버퍼 = 픽셀마다 "카메라에서 얼마나 먼지"를 적어 두는 메모장. 평소엔 그리면서 적는 용도(DEPTH_WRITE).
    // 1) DEPTH_WRITE -> PIXEL_SHADER_RESOURCE: "이제 셰이더가 이 메모장을 텍스처처럼 읽을 거야"로 용도 변경
    //    (진짜 게임에선 그림자 맵, SSAO처럼 깊이를 읽어야 할 때 하는 일)
    // 2) PIXEL_SHADER_RESOURCE -> DEPTH_WRITE: 바로 다시 "적는 용도"로 되돌림
    // 그런데 그 사이에 아무도 읽지 않는다 = 쓸모없는 왕복. 그래도 GPU는 바꿀 때마다
    // 앞 드로우가 끝날 때까지 기다리고(플러시), 캐시를 비우고, 깊이 압축을 풀었다 다시 묶을 수 있다 -> 시간 낭비
    D3D12_RESOURCE_BARRIER x = Transition(g_depth->res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    f.cl->ResourceBarrier(1, &x);
    D3D12_RESOURCE_BARRIER y = Transition(g_depth->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    f.cl->ResourceBarrier(1, &y);
    f.st.barriers += 2;
}

// ===== [시나리오 5] 최적: 인스턴싱 (드로우 몇 번으로 큐브 전부) =====
static void DrawCubesInstanced(FrameCtx& f)
{
    const Settings& s = *f.s;
    int count = f.count;
    int segs = std::min(s.uselessBarriers + 1, count);   // 배리어를 끼울 자리만큼 나눠 그림
    for (int g = 0; g < segs; g++)
    {
        int a = (int)((int64_t)count * g / segs), e = (int)((int64_t)count * (g + 1) / segs);
        if (g > 0) RecordUselessBarrier(f);
        if (e <= a) continue;
        UINT rc[4] = { (UINT)a, 0, 0, 0 };
        f.cl->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
        f.cl->DrawIndexedInstanced(36, (UINT)(e - a), 0, 0, 0);
        f.st.drawCalls++;
    }
    for (int g = segs; g <= s.uselessBarriers; g++) RecordUselessBarrier(f);  // 큐브보다 배리어가 많을 때
}

// ===== [시나리오 5] 최악: 물체마다 드로우 =====
static void DrawCubesOneByOne(FrameCtx& f, const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv)
{
    const Settings& s = *f.s;
    ID3D12GraphicsCommandList* cl = f.cl;
    int count = f.count;
    int every = s.uselessBarriers > 0 ? std::max(1, count / s.uselessBarriers) : INT32_MAX;
    int done = 0;
    for (int i = 0; i < count; i++)
    {
        // ===== [시나리오 6] 최악: 드로우마다 PSO + 루트 시그니처 + 바인딩 전부 다시 =====
        if (s.resetStatePerDraw)
        {
            SetCommonState(f.st, cl, *f.fr, f.instVA, f.vp, f.sc, f.rtv, f.dsv);
            cl->SetPipelineState((i & 1) ? f.psoB : f.psoA); f.st.psoSets++;
            cl->IASetVertexBuffers(0, 1, &vbv);
            cl->IASetIndexBuffer(&ibv);
        }
        UINT rc[4] = { (UINT)i, s.bufferMode == BM_NEW_PER_OBJECT ? 1u : 0u, 0, 0 };
        cl->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
        if (s.bufferMode == BM_NEW_PER_OBJECT) cl->SetGraphicsRootConstantBufferView(3, f.objCB[i]);
        cl->DrawIndexedInstanced(36, 1, 0, 0, 0);
        f.st.drawCalls++;
        if (done < s.uselessBarriers && (i + 1) % every == 0) { RecordUselessBarrier(f); done++; }
    }
    for (; done < s.uselessBarriers; done++) RecordUselessBarrier(f);
}

// 7-2) 큐브
static void DrawCubes(FrameCtx& f)
{
    const Settings& s = *f.s;
    int count = f.count;
    if (count <= 0)
    {
        for (int g = 0; g < s.uselessBarriers; g++) RecordUselessBarrier(f);
        return;
    }
    f.st.uniqueVerts += 24ull * count; f.st.uniqueTris += 12ull * count;        // 큐브 1개 = 정점 24개(면마다 4개), 삼각형 12개
    f.st.submittedVerts += 36ull * count; f.st.submittedTris += 12ull * count;  // 인덱스 36개를 보냄
    D3D12_VERTEX_BUFFER_VIEW vbv{ g_cubeVB->VA(), (UINT)g_cubeVB->size, sizeof(Vertex) };
    D3D12_INDEX_BUFFER_VIEW ibv{ g_cubeIB->VA(), (UINT)g_cubeIB->size, DXGI_FORMAT_R16_UINT };
    f.cl->IASetVertexBuffers(0, 1, &vbv);
    f.cl->IASetIndexBuffer(&ibv);
    // ===== [시나리오 5] 인스턴싱(드로우 1번) vs 물체마다 드로우 =====
    bool individual = !s.instancing || s.bufferMode == BM_NEW_PER_OBJECT;
    if (!individual) DrawCubesInstanced(f);
    else DrawCubesOneByOne(f, vbv, ibv);
}

// 7-3) VRAM 압박: 각 덩어리에서 256바이트씩 복사 -> 모든 덩어리가 VRAM에 있어야 함
// ===== [시나리오 9] 매 프레임 사용: 덩어리를 실제로 읽어서 VRAM에 있어야 하게 만듦 =====
static void TouchPressureChunks(FrameCtx& f)
{
    if (!f.s->pressureTouch || g_pressure.empty()) return;
    static size_t cursor = 0;
    size_t n = std::min<size_t>(g_pressure.size(), kPressureTouchPerFrame);
    for (size_t k = 0; k < n; k++)
    {
        size_t i = (cursor + k) % g_pressure.size();
        f.cl->CopyBufferRegion(g_pressureSink->res.Get(), k * 256, g_pressure[i]->res.Get(), 0, 256);
    }
    cursor = (cursor + n) % g_pressure.size();
}

// 7-4) 화면 되읽기: 백 버퍼 -> Readback 힙
// ===== [시나리오 7] 화면(백 버퍼)을 Readback 힙으로 복사 =====
static void CopySceneToReadback(FrameCtx& f)
{
    FrameRes& fr = *f.fr;
    if (f.s->readback == RB_OFF) { fr.rbValid = false; return; }
    ID3D12GraphicsCommandList* cl = f.cl;
    if (!fr.sceneReadback) fr.sceneReadback = MustBuffer(HK_READBACK, g_rbBytes, true);
    D3D12_RESOURCE_BARRIER a = Transition(f.back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &a);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = fr.sceneReadback->res.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = g_rbFootprint;
    src.pResource = f.back; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = Transition(f.back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, &b);
    f.st.barriers += 2;
    fr.rbValid = true;
}

// 7-5) UI 그리기 + 백 버퍼를 "Present"로 + 측정값 정리 + 명령 목록 닫기
static void RecordUiAndClose(FrameCtx& f)
{
    FrameRes& fr = *f.fr;
    ID3D12GraphicsCommandList* cl = f.cl;
    D3D12_VIEWPORT full{ 0, 0, (float)g_width, (float)g_height, 0, 1 };
    cl->RSSetViewports(1, &full);
    cl->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);

    D3D12_RESOURCE_BARRIER b1 = Transition(f.back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, &b1); f.st.barriers++;
    cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, f.tsBase + 3);
    cl->ResolveQueryData(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, f.tsBase, kTsPerFrame, fr.tsReadback->res.Get(), 0);
    if (g_pipeStatsHeap) cl->ResolveQueryData(g_pipeStatsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, f.fi, 1, fr.statsReadback->res.Get(), 0);
    fr.statsValid = g_pipeStatsHeap != nullptr;
    fr.tsValid = true;
    HR(cl->Close());
}

static void PopulateCommandList(FrameCtx& f)
{
    BeginCommandList(f);
    RecordTextureRecreate(f);
    SetCommonState(f.st, f.cl, *f.fr, f.instVA, f.vp, f.sc, f.rtv, f.dsv);
    f.cl->SetPipelineState(f.psoA); f.st.psoSets++;

    DrawPlanet(f);
    f.cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, f.tsBase + 1);
    DrawCubes(f);
    if (g_pipeStatsHeap) f.cl->EndQuery(g_pipeStatsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, f.fi);
    f.cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, f.tsBase + 2);

    TouchPressureChunks(f);
    CopySceneToReadback(f);
    RecordUiAndClose(f);
}

// ---------------------------------------------------------------------
// 8) ~ 10) 제출, Present, 통계
// ---------------------------------------------------------------------
static void SubmitAndPresent(FrameCtx& f)
{
    double tp0 = NowMs();
    ID3D12CommandList* lists[] = { f.cl };
    g_directQueue->ExecuteCommandLists(1, lists);
    // ===== [시나리오 8] Present: VSync 켬 = 1(모니터 기다림), 끔 = 0 =====
    HRESULT phr = g_swapChain->Present(f.s->vsync ? 1 : 0, (!f.s->vsync && g_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET)
    {
        char b[160]; snprintf(b, sizeof b, "GPU 장치가 제거됨 (reason 0x%08X)", (unsigned)g_device->GetDeviceRemovedReason());
        ShowMessage(b); ExitProcess(1);
    }
    HR(g_directQueue->Signal(g_fence.Get(), ++g_fenceValue));
    g_frameFence[g_frameNo % 8] = g_fenceValue;
    f.presentMs = NowMs() - tp0;
}

// 바로 기다리는 되읽기: CPU가 여기서 멈춤
// ===== [시나리오 7] 최악: 복사 시키자마자 펜스로 GPU를 기다림 =====
static void WaitReadbackNow(FrameCtx& f)
{
    if (f.s->readback != RB_WAIT_NOW) return;
    double t0 = NowMs();
    WaitFence(g_fence.Get(), g_fenceValue);
    f.waitMs += NowMs() - t0;
    ReadSceneColor(*f.fr);
    g_rbAgeFrames = 0;
}

static void UpdateStatsAndHistory(FrameCtx& f)
{
    Stats& st = f.st;
    st.gpuStatsValid = g_stats.gpuStatsValid; st.gpuIAVerts = g_stats.gpuIAVerts; st.gpuVSInvocations = g_stats.gpuVSInvocations;
    st.gpuIAPrims = g_stats.gpuIAPrims; st.gpuRasterPrims = g_stats.gpuRasterPrims; st.gpuPSInvocations = g_stats.gpuPSInvocations;
    st.frameMs = g_stats.frameMs; st.gpuTotalMs = g_stats.gpuTotalMs; st.gpuMeshMs = g_stats.gpuMeshMs; st.gpuCubesMs = g_stats.gpuCubesMs;
    st.waitMs = f.waitMs; st.cpuWorkMs = f.cpuWorkMs; st.presentMs = f.presentMs;
    st.created = g_resourcesCreatedThisFrame;
    g_stats = st;
    if (g_autotest) AutotestLogStats();
    UpdateAverage(NowMs());
    QueryMemory();
    g_histFrame[g_histPos] = (float)g_stats.frameMs;
    g_histGpu[g_histPos] = (float)g_stats.gpuTotalMs;
    g_histVram[g_histPos] = (float)(g_mem.local.CurrentUsage >> 20);
    g_histRam[g_histPos] = (float)(g_mem.privateBytes >> 20);
    g_histPos = (g_histPos + 1) % kHist;
}

static void RenderFrame()
{
    FrameCtx f;
    f.s = &g_s;
    BeginFrameTiming(f);
    ApplyPendingRebuilds(*f.s);
    WaitForFrameSlot(f);
    ReadFinishedFrameResults(f);
    BuildUi(f);
    PrepareCpuData(f);
    PopulateCommandList(f);
    f.cpuWorkMs = NowMs() - f.cpuWorkStart;
    SubmitAndPresent(f);
    WaitReadbackNow(f);
    g_frameNo++;
    UpdateStatsAndHistory(f);
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
        if (wParam != SIZE_MINIMIZED && g_device)
        {
            g_newW = LOWORD(lParam); g_newH = HIWORD(lParam);
            if (g_newW != g_width || g_newH != g_height) g_resizePending = true;
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

// 창 만들기 (Samples 의 Win32Application::Run 앞부분)
static void CreateMainWindow(HINSTANCE hInst, float dpi)
{
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"DX12MemDemo", nullptr };
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"DX12 메모리 최적화 데모", WS_OVERLAPPEDWINDOW, 60, 60,
                           (int)(1600 * dpi), (int)(900 * dpi), nullptr, nullptr, hInst, nullptr);
}

// ImGui 준비: 스타일, 한글 글꼴, Win32 + DX12 백엔드
static void InitImGui(float dpi)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpi);
    style.FontScaleDpi = dpi;
    style.FontSizeBase = 17.0f;
    style.WindowRounding = 0.0f;
    // 한글 표시: 맑은 고딕 (1.92부터 글자를 필요할 때 만들어서 한글 전체 범위도 가볍다)
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf"))
        io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g_device.Get();
    ii.CommandQueue = g_directQueue.Get();
    ii.NumFramesInFlight = kMaxFrames;
    ii.RTVFormat = kBackBufferFormat;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_srvHeap.Get();
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) { g_srvAlloc.Alloc(c, g); };
    ii.SrvDescriptorFreeFn  = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE g) { g_srvAlloc.Free(c, g); };
    ImGui_ImplDX12_Init(&ii);
}

// 메시지 처리 + 매 프레임 RenderFrame (창이 닫힐 때까지)
static void RunMessageLoop()
{
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
        if (IsIconic(g_hwnd)) { Sleep(16); continue; }
        RenderFrame();
    }
}

static void Shutdown()
{
    WaitIdle();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    CreateMainWindow(hInst, dpi);
    InitD3D();
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    InitImGui(dpi);

    PresetBest(g_s);
    g_autotest = GetEnvironmentVariableA("DEMO_AUTOTEST", nullptr, 0) > 0;
    g_uiMeshTrisK = g_s.meshTrisK; g_uiPressureMB = g_s.pressureMB;
    QueryMemory();

    RunMessageLoop();
    Shutdown();
    return 0;
}
