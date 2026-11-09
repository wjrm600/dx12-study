// =====================================================================
//  장면 1: 래스터라이저 - 삼각형이 어떤 픽셀을 칠하는지
//
//  화면의 큰 칸 하나 = 진짜 픽셀 하나라고 생각하자 (돋보기로 크게 본 화면).
//  꼭짓점 A(빨강), B(초록), C(파랑)를 마우스로 끌면
//    - CPU 가 칸마다 "가운데 점이 삼각형 안인가?"를 외적으로 직접 계산하고 (무게 wa, wb, wc)
//    - GPU 가 같은 삼각형을 진짜 DX12 파이프라인으로 그린다.
//  두 결과를 칸마다 비교해서 "GPU 도 똑같은 계산을 한다"는 걸 눈으로 확인한다.
//
//  코드에서 Ctrl+F 로 찾기:
//    [래스터-①] CPU 래스터라이저: 외적으로 넓이, 무게(wa, wb, wc), 안/밖 판정
//    [래스터-②] 탑-레프트 규칙
//    [래스터-③] 컬링: PSO 의 CullMode, 넓이 부호로 앞/뒷면
//    [래스터-④] GPU 1패스: 작은 렌더 타깃에 진짜 삼각형 그리기 (루트 상수 + SV_VertexID)
//    [래스터-⑤] GPU 2패스: 작은 그림을 화면에 크게 늘려 그리기 (POINT 샘플러)
//    [래스터-⑥] 되읽기: GPU 결과를 CPU 로 가져와 칸마다 비교
// =====================================================================
#include "../scene.h"
#include "../shader_file.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

static const int kMinCols = 6, kMaxCols = 64, kMaxRows = 64;

struct P2 { double x, y; };
struct Tri { P2 v[3]; };   // v[0]=A(빨강), v[1]=B(초록), v[2]=C(파랑). 단위: 칸(픽셀). y 는 아래로 갈수록 커진다.

enum ViewMode { VIEW_CPU, VIEW_GPU, VIEW_COMPARE };
enum CullKind { CULLK_NONE, CULLK_BACK, CULLK_FRONT };

// GPU 는 꼭짓점 좌표를 픽셀의 1/256 단위로 반올림한다 (서브픽셀 정밀도 8비트).
// CPU 계산도 똑같이 반올림해야 경계에 걸린 칸까지 결과가 같아진다.
static double Snap256(double v) { return std::floor(v * 256.0 + 0.5) / 256.0; }

// 외적 (2D): 세 점 o, a, b 가 만드는 삼각형 넓이의 2배. 부호 = 돌아가는 방향.
//   화면 좌표(y 가 아래로 커짐)에서  + 이면 시계 방향,  - 이면 반시계 방향.
static double Cross(P2 o, P2 a, P2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// ===== [래스터-②] 탑-레프트 규칙: 픽셀 중심이 모서리 위에 "딱" 걸치면 누가 칠하나 =====
// 모서리 from -> to 를 시계 방향 순서로 볼 때
//   위쪽 모서리 = 수평(dy == 0)이고 오른쪽으로 감(dx > 0)
//   왼쪽 모서리 = 위로 올라감(dy < 0)   (화면 좌표라 위로 = y 감소)
static bool IsTopLeftEdge(P2 from, P2 to, bool clockwise)
{
    double dx = to.x - from.x, dy = to.y - from.y;
    if (!clockwise) { dx = -dx; dy = -dy; }      // 반시계 삼각형이면 방향을 뒤집어서 같은 규칙을 쓴다
    bool top = (dy == 0.0 && dx > 0.0);
    bool left = (dy < 0.0);
    return top || left;
}

struct CellCalc
{
    double area2 = 0;      // 삼각형 ABC 넓이의 2배 (외적). + 시계(앞면), - 반시계(뒷면)
    double e[3] = {};      // 조각 넓이의 2배: e[0]=PBC(A의 반대편), e[1]=APC(B의 반대편), e[2]=ABP(C의 반대편)
    double w[3] = {};      // 무게: wa, wb, wc = 조각 넓이 / 전체 넓이  (합 = 1)
    bool   front = false;  // 앞면?
    bool   culled = false; // 컬링돼서 통째로 버려짐?
    bool   onEdge[3] = {}; // 무게가 정확히 0 (모서리 위)
    bool   inside = false; // 이 칸을 칠하나?
};

// ===== [래스터-①] CPU 래스터라이저: 칸 하나(픽셀 중심 P)를 판정한다 =====
static CellCalc Evaluate(const Tri& t, P2 p, int cull, bool topLeftRule)
{
    CellCalc r;
    const P2 A = t.v[0], B = t.v[1], C = t.v[2];
    r.area2 = Cross(A, B, C);
    r.e[0] = Cross(p, B, C);   // P 와 B, C 가 만드는 조각 -> A 의 몫
    r.e[1] = Cross(A, p, C);   // A, P, C -> B 의 몫
    r.e[2] = Cross(A, B, p);   // A, B, P -> C 의 몫
    if (r.area2 != 0.0)
        for (int i = 0; i < 3; i++) r.w[i] = r.e[i] / r.area2;

    // ===== [래스터-③] 컬링: 넓이 부호만 보고 삼각형을 통째로 버린다 (칸 계산 전에!) =====
    // DX12 기본값 FrontCounterClockwise = FALSE -> 화면에서 시계 방향이 앞면
    r.front = r.area2 > 0.0;
    r.culled = (r.area2 == 0.0) || (cull == CULLK_BACK && !r.front) || (cull == CULLK_FRONT && r.front);
    if (r.culled) return r;

    // 안/밖: 세 조각이 모두 전체와 같은 부호(무게 >= 0)면 안쪽
    const P2 edgeFrom[3] = { B, C, A }, edgeTo[3] = { C, A, B };   // 꼭짓점 i 의 맞은편 모서리
    bool inside = true;
    for (int i = 0; i < 3; i++)
    {
        double s = r.front ? r.e[i] : -r.e[i];
        r.onEdge[i] = (s == 0.0);
        if (s < 0.0) inside = false;
        else if (s == 0.0 && topLeftRule && !IsTopLeftEdge(edgeFrom[i], edgeTo[i], r.front)) inside = false;
    }
    r.inside = inside;
    return r;
}

static unsigned char ToByte(double v) { v = std::min(1.0, std::max(0.0, v)); return (unsigned char)std::floor(v * 255.0 + 0.5); }

// ---------------------------------------------------------------------
// 셰이더는 lab/shaders/raster.hlsl 파일에 있다. 고치고 저장하면 BuildPipelines() 가 다시 불린다.
// ---------------------------------------------------------------------

struct TriConstants { float a[2], b[2], c[2], grid[2], colA[4], colB[4], colC[4]; };
static_assert(sizeof(TriConstants) == 80, "root constants = 20 x 4 bytes");

// 되읽기 한 칸 결과
struct Compare
{
    bool valid = false;
    int  cols = 0, rows = 0;
    int  cpuCount = 0, gpuCount = 0, diffCount = 0, maxColorDiff = 0;
    int  caseId = -1;
    std::vector<unsigned char> gpu;     // 칸마다 RGBA (4바이트)
    std::vector<unsigned char> diff;    // 칸마다 1 = CPU 와 GPU 가 다름
};

class RasterScene : public Scene
{
public:
    const char* Name() const override { return "장면 1: 래스터라이저 (무게중심, 외적, 컬링)"; }
    void Init() override;
    void Shutdown() override;
    void OnFrameReused(UINT slot) override;
    void OnPanel() override;
    void OnCanvas(const Canvas& c) override;
    void OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c) override;
    bool AutoTest(int frame, int& failures) override;

private:
    // 설정
    int  m_view = VIEW_CPU;
    int  m_cols = 24, m_rows = 14;
    int  m_snap = 1;                // 0: 자유(1/256칸), 1: 0.5칸, 2: 1칸
    int  m_cull = CULLK_NONE;
    bool m_topLeft = true;
    Tri  m_tri{};
    bool m_shapeReady = false;

    // 화면 배치 (OnCanvas 에서 계산)
    float m_gx0 = 0, m_gy0 = 0, m_cell = 20;
    int   m_drag = -1;
    int   m_selX = -1, m_selY = -1;
    bool  m_pinned = false;

    // GPU 자원
    ShaderFile                  m_sh;              // lab/shaders/raster.hlsl
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_triPso[3];       // 컬링 없음 / 뒷면 / 앞면
    ComPtr<ID3D12PipelineState> m_blitPso;
    ComPtr<ID3D12Resource>      m_low;             // 작은 렌더 타깃 (칸 수 x 칸 수 픽셀)
    int m_lowCols = 0, m_lowRows = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE m_lowRtv{}, m_lowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_lowSrvGpu{};

    struct Slot
    {
        ComPtr<ID3D12Resource> readback;   // Readback 힙 (시스템 RAM): GPU -> CPU 로 가져오는 상자
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        bool valid = false;
        Tri tri{}; int cols = 0, rows = 0, cull = 0; bool topLeft = true; int caseId = -1;
    };
    Slot m_slots[kFrameCount];
    Compare m_cmp;

    // 자동 테스트
    int m_caseId = 0;
    int m_testCase = -1, m_testWait = 0;

    void ResetShape();
    void EdgeShape();
    void SetCols(int cols);
    void ClampAndSnap();
    double SnapStep() const { return m_snap == 0 ? 1.0 / 256.0 : (m_snap == 1 ? 0.5 : 1.0); }
    void CreateLowRes(int cols, int rows);
    bool BuildPipelines();
    ImVec2 ToScreen(P2 p) const { return ImVec2(m_gx0 + (float)p.x * m_cell, m_gy0 + (float)p.y * m_cell); }
    void ApplyTestCase(int k);
    bool CheckAndLogTestCase();
    bool NextTestCase();

    // Init 단계
    struct ShaderBlobs { ComPtr<ID3DBlob> triVS, triPS, blitVS, blitPS; };
    void CreateRootSig();
    void ReadStartViewFromEnv();
    void CreateReadbackBuffers();
    bool CompileShaders(ShaderBlobs& b, std::string& err);
    bool CreatePsos(const ShaderBlobs& b, ComPtr<ID3D12PipelineState> tri[3], ComPtr<ID3D12PipelineState>& blit);

    // OnCanvas 단계
    float GridW() const { return m_cols * m_cell; }
    float GridH() const { return m_rows * m_cell; }
    P2   SelectedCenter() const { return P2{ m_selX + 0.5, m_selY + 0.5 }; }
    bool CompareReady() const { return m_cmp.valid && m_cmp.cols == m_cols && m_cmp.rows == m_rows; }
    void LayoutGrid(const Canvas& c);
    void BeginCanvasWindow(const Canvas& c);
    int  PickVertex(ImVec2 m) const;
    void HandleMouse();
    void DrawCells(ImDrawList* dl) const;
    void DrawGridLines(ImDrawList* dl) const;
    void DrawSelectedCell(ImDrawList* dl) const;
    void DrawTriangleEdges(ImDrawList* dl, bool culled) const;
    void DrawVertices(ImDrawList* dl) const;
    void DrawStatusText(ImDrawList* dl, const Canvas& c, const CellCalc& whole) const;

    // OnRender 단계
    TriConstants MakeConstants() const;
    void SetRootArguments(ID3D12GraphicsCommandList* cl);
    void RenderTriangleToLowRes(ID3D12GraphicsCommandList* cl);
    void CopyLowResToReadback(ID3D12GraphicsCommandList* cl);
    void DrawLowResToScreen(ID3D12GraphicsCommandList* cl);

    // OnPanel 단계
    void PanelSettings();
    void PanelShader();
    void PanelWholeTriangle(const CellCalc& cc);
    void PanelCellTable(const CellCalc& cc);
    void PanelCellVerdict(const CellCalc& cc);
    void PanelCellColors(const CellCalc& cc);
    void PanelCompare();
    void PanelEli5();
    void PanelCodeFlow();
};

// ---------------------------------------------------------------------
// 모양
// ---------------------------------------------------------------------
void RasterScene::ResetShape()
{
    // A 위, B 오른쪽 아래, C 왼쪽 아래 -> 화면에서 시계 방향 = 앞면
    m_tri.v[0] = { m_cols * 0.50, m_rows * 0.10 };
    m_tri.v[1] = { m_cols * 0.86, m_rows * 0.88 };
    m_tri.v[2] = { m_cols * 0.14, m_rows * 0.70 };
    ClampAndSnap();
    m_caseId++;
}

// 탑-레프트 규칙 실험용: 모서리가 픽셀 중심을 정확히 지나가는 모양
void RasterScene::EdgeShape()
{
    double x0 = 2.5, y0 = 2.5;
    double s = std::max(3.0, std::floor(std::min(m_cols - 5.0, m_rows - 5.0)));
    m_tri.v[0] = { x0, y0 };          // 왼쪽 위
    m_tri.v[1] = { x0 + s, y0 };      // 오른쪽 위 (A-B = 위쪽 수평 모서리)
    m_tri.v[2] = { x0, y0 + s };      // 왼쪽 아래 (C-A = 왼쪽 세로 모서리)
    m_snap = 1;
    ClampAndSnap();
    m_caseId++;
}

void RasterScene::SetCols(int cols)
{
    cols = std::max(kMinCols, std::min(kMaxCols, cols));
    if (cols == m_cols) return;
    double k = (double)cols / (double)m_cols;   // 칸 크기가 바뀌어도 모양은 그대로 보이게 좌표를 같이 늘린다
    for (auto& v : m_tri.v) { v.x *= k; v.y *= k; }
    m_cols = cols;
    m_rows = std::max(1, (int)std::floor(m_rows * k));
    ClampAndSnap();
    m_caseId++;
}

void RasterScene::ClampAndSnap()
{
    double step = SnapStep();
    for (auto& v : m_tri.v)
    {
        v.x = std::floor(v.x / step + 0.5) * step;
        v.y = std::floor(v.y / step + 0.5) * step;
        v.x = Snap256(std::max(0.0, std::min((double)m_cols, v.x)));
        v.y = Snap256(std::max(0.0, std::min((double)m_rows, v.y)));
    }
}

// ---------------------------------------------------------------------
// GPU 자원
// ---------------------------------------------------------------------
void RasterScene::Init()
{
    CreateRootSig();                 // 1) 셰이더에 무엇을 넘기는지 (루트 상수 b0, SRV t0, 샘플러 s0)
    m_sh.Open("raster.hlsl");        // 2) 셰이더 파일 읽기
    BuildPipelines();                //    컴파일 + PSO (실패해도 앱은 계속)
    ReadStartViewFromEnv();
    CreateReadbackBuffers();         // 3) 되읽기 상자 + 작은 그림용 서술자 자리
    CreateLowRes(m_cols, m_rows);    // 4) 작은 렌더 타깃
}

void RasterScene::CreateRootSig()
{
    // 루트 시그니처: [0] 루트 상수 20개(b0)  [1] 표: SRV 1개(t0)  + 고정 샘플러 s0 (POINT)
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1; range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = sizeof(TriConstants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2; rs.pParameters = params; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    m_rootSig = CreateRootSignature(rs);
}

void RasterScene::ReadStartViewFromEnv()
{
    char env[8] = {};   // 개발용: LAB_VIEW=0/1/2 로 처음 보기 모드를 고른다 (스크린샷 확인용)
    if (GetEnvironmentVariableA("LAB_VIEW", env, sizeof env) > 0) m_view = std::max(0, std::min(2, atoi(env)));
}

void RasterScene::CreateReadbackBuffers()
{
    m_lowRtv = AllocRtv();
    AllocSrv(&m_lowSrvCpu, &m_lowSrvGpu);
    for (auto& s : m_slots)
        s.readback = CreateBuffer(D3D12_HEAP_TYPE_READBACK, 256ull * kMaxRows, D3D12_RESOURCE_STATE_COPY_DEST);
}

// 셰이더 4개 컴파일. 하나라도 틀리면 false 와 에러 메시지
bool RasterScene::CompileShaders(ShaderBlobs& b, std::string& err)
{
    bool ok = TryCompile(m_sh, "TriVS", "vs_5_0", b.triVS, err);
    ok &= TryCompile(m_sh, "TriPS", "ps_5_0", b.triPS, err);
    ok &= TryCompile(m_sh, "BlitVS", "vs_5_0", b.blitVS, err);
    ok &= TryCompile(m_sh, "BlitPS", "ps_5_0", b.blitPS, err);
    return ok;
}

// 두 패스가 같이 쓰는 PSO 기본값 (셰이더와 컬링만 나중에 바꿔 끼운다)
static D3D12_GRAPHICS_PIPELINE_STATE_DESC BasePsoDesc(ID3D12RootSignature* rootSig)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig;
    pd.InputLayout = { nullptr, 0 };                 // 정점 버퍼 없음
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.FrontCounterClockwise = FALSE; // 기본값: 화면에서 시계 방향 = 앞면
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = kBackBufferFormat;
    pd.SampleDesc.Count = 1;
    return pd;
}

bool RasterScene::CreatePsos(const ShaderBlobs& b, ComPtr<ID3D12PipelineState> tri[3], ComPtr<ID3D12PipelineState>& blit)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = BasePsoDesc(m_rootSig.Get());
    bool ok = true;

    // ===== [래스터-③] 컬링은 PSO 의 래스터라이저 상태에 들어 있다 (그래서 바꾸려면 PSO 를 바꾼다) =====
    const D3D12_CULL_MODE modes[3] = { D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_FRONT };
    pd.VS = { b.triVS->GetBufferPointer(), b.triVS->GetBufferSize() };
    pd.PS = { b.triPS->GetBufferPointer(), b.triPS->GetBufferSize() };
    for (int i = 0; i < 3; i++)
    {
        pd.RasterizerState.CullMode = modes[i];
        if (FAILED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tri[i])))) ok = false;
    }
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.VS = { b.blitVS->GetBufferPointer(), b.blitVS->GetBufferSize() };
    pd.PS = { b.blitPS->GetBufferPointer(), b.blitPS->GetBufferSize() };
    if (FAILED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&blit)))) ok = false;
    return ok;
}

bool RasterScene::BuildPipelines()
{
    // 컴파일 -> PSO 만들기 -> 교체. 중간에 실패하면 에러를 편집 창에 보여 주고, 지금 쓰던 PSO 를 그대로 둔다.
    ShaderBlobs blobs;
    std::string err;
    if (!CompileShaders(blobs, err)) { m_sh.error = err; return false; }

    ComPtr<ID3D12PipelineState> tri[3], blit;
    if (!CreatePsos(blobs, tri, blit))
    {
        m_sh.error = "컴파일은 됐지만 PSO 를 못 만들었어요.\n"
                     "흔한 이유: VS 가 내보내는 값(VSOut)과 PS 가 받는 값이 안 맞음, 또는 루트 시그니처에 없는 레지스터(b1, t1 ...)를 씀.\n";
        return false;
    }

    // 새 PSO 로 교체: 옛 PSO 는 GPU 가 아직 쓰고 있을 수 있어서, 다 끝날 때까지 기다린 뒤 바꾼다
    if (m_blitPso) WaitIdle();
    for (int i = 0; i < 3; i++) m_triPso[i] = tri[i];
    m_blitPso = blit;
    m_sh.error.clear();
    m_sh.buildCount++;
    m_caseId++;   // GPU 결과가 바뀌었으니 비교를 새로
    return true;
}

void RasterScene::Shutdown()
{
    FreeRtv(m_lowRtv);
    FreeSrv(m_lowSrvCpu);
}

void RasterScene::CreateLowRes(int cols, int rows)
{
    if (m_low) WaitIdle();     // GPU 가 옛 텍스처를 다 쓴 뒤에 바꾼다
    m_low.Reset();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;   // VRAM
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = (UINT64)cols; d.Height = (UINT)rows;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = kBackBufferFormat; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{}; cv.Format = kBackBufferFormat;   // (0,0,0,0) = 투명 = "안 칠함"
    HR(g_dx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&m_low)));
    g_dx.device->CreateRenderTargetView(m_low.Get(), nullptr, m_lowRtv);
    g_dx.device->CreateShaderResourceView(m_low.Get(), nullptr, m_lowSrvCpu);
    m_lowCols = cols; m_lowRows = rows;
    for (auto& s : m_slots)
    {
        UINT nrows; UINT64 rowBytes, total;
        g_dx.device->GetCopyableFootprints(&d, 0, 1, 0, &s.fp, &nrows, &rowBytes, &total);
    }
}

// ===== [래스터-⑥] 되읽기: 2프레임 전에 GPU 가 그린 작은 그림을 CPU 가 읽고 칸마다 비교 =====
void RasterScene::OnFrameReused(UINT slot)
{
    Slot& s = m_slots[slot];
    if (!s.valid) return;
    s.valid = false;
    const unsigned char* data = nullptr;
    D3D12_RANGE rr{ 0, (SIZE_T)(s.fp.Footprint.RowPitch * s.rows) };
    if (FAILED(s.readback->Map(0, &rr, (void**)&data))) return;

    Compare c;
    c.valid = true; c.cols = s.cols; c.rows = s.rows; c.caseId = s.caseId;
    c.gpu.assign((size_t)s.cols * s.rows * 4, 0);
    c.diff.assign((size_t)s.cols * s.rows, 0);
    for (int y = 0; y < s.rows; y++)
        for (int x = 0; x < s.cols; x++)
        {
            const unsigned char* px = data + (size_t)y * s.fp.Footprint.RowPitch + (size_t)x * 4;
            unsigned char* g = &c.gpu[((size_t)y * s.cols + x) * 4];
            g[0] = px[0]; g[1] = px[1]; g[2] = px[2]; g[3] = px[3];
            bool gpuIn = px[3] >= 128;
            CellCalc cc = Evaluate(s.tri, P2{ x + 0.5, y + 0.5 }, s.cull, s.topLeft);
            c.cpuCount += cc.inside; c.gpuCount += gpuIn;
            if (gpuIn != cc.inside) { c.diffCount++; c.diff[(size_t)y * s.cols + x] = 1; }
            else if (gpuIn)
            {
                for (int k = 0; k < 3; k++) c.maxColorDiff = std::max(c.maxColorDiff, std::abs((int)px[k] - (int)ToByte(cc.w[k])));
            }
        }
    D3D12_RANGE none{ 0, 0 };
    s.readback->Unmap(0, &none);
    m_cmp = std::move(c);
}

// ---------------------------------------------------------------------
// 왼쪽 그림 영역: 격자, 삼각형, 마우스
// ---------------------------------------------------------------------
void RasterScene::OnCanvas(const Canvas& c)
{
    LayoutGrid(c);                   // 1) 칸 크기, 격자 위치
    BeginCanvasWindow(c);
    HandleMouse();                   // 2) 꼭짓점 끌기, 칸 고르기
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawCells(dl);                   // 3) 칸 칠하기 (CPU 계산 / 비교 표시)
    DrawGridLines(dl);
    DrawSelectedCell(dl);            // 4) 고른 칸의 세 조각
    const CellCalc whole = Evaluate(m_tri, SelectedCenter(), m_cull, m_topLeft);
    DrawTriangleEdges(dl, whole.culled);   // 5) 삼각형 모서리 + 꼭짓점
    DrawVertices(dl);
    DrawStatusText(dl, c, whole);    // 6) 위쪽 안내 글
    ImGui::End();

    DrawShaderEditor(m_sh, c);       // 셰이더 편집 창 (F2 로 열기/닫기)
}

void RasterScene::LayoutGrid(const Canvas& c)
{
    const float fs = ImGui::GetFontSize();
    const float margin = fs * 1.0f, top = fs * 3.4f;
    // 칸 크기: 가로 칸 수로 정하고, 세로 칸 수는 남는 높이에 맞춘다 (칸은 정사각형)
    m_cell = std::max(2.0f, (c.w - margin * 2) / (float)m_cols);
    int rows = std::max(1, std::min(kMaxRows, (int)std::floor((c.h - top - margin) / m_cell)));
    if (rows != m_rows) { m_rows = rows; ClampAndSnap(); m_caseId++; }
    if (!m_shapeReady) { ResetShape(); m_shapeReady = true; }
    m_gx0 = c.x + margin;
    m_gy0 = c.y + top;
}

void RasterScene::BeginCanvasWindow(const Canvas& c)
{
    ImGui::SetNextWindowPos(ImVec2(c.x, c.y));
    ImGui::SetNextWindowSize(ImVec2(c.w, c.h));
    ImGui::Begin("##canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
}

// 누른 곳에서 가장 가까운 꼭짓점 (손가락 굵기 안에 없으면 -1)
int RasterScene::PickVertex(ImVec2 m) const
{
    int pick = -1;
    float best = ImGui::GetFontSize() * 0.9f;
    for (int i = 0; i < 3; i++)
    {
        ImVec2 s = ToScreen(m_tri.v[i]);
        float d = std::sqrt((s.x - m.x) * (s.x - m.x) + (s.y - m.y) * (s.y - m.y));
        if (d < best) { best = d; pick = i; }
    }
    return pick;
}

void RasterScene::HandleMouse()
{
    ImGui::SetCursorScreenPos(ImVec2(m_gx0, m_gy0));
    ImGui::InvisibleButton("grid", ImVec2(GridW(), GridH()));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int cx = (int)std::floor((m.x - m_gx0) / m_cell), cy = (int)std::floor((m.y - m_gy0) / m_cell);
    if (ImGui::IsItemActivated())
    {
        m_drag = PickVertex(m);
        if (m_drag < 0)   // 꼭짓점이 아닌 칸을 누르면 그 칸을 고정 / 해제
        {
            if (m_pinned && cx == m_selX && cy == m_selY) m_pinned = false;
            else { m_pinned = true; m_selX = cx; m_selY = cy; }
        }
    }
    if (m_drag >= 0 && ImGui::IsItemActive())
    {
        m_tri.v[m_drag] = { (m.x - m_gx0) / m_cell, (m.y - m_gy0) / m_cell };
        ClampAndSnap();
        m_caseId++;
    }
    if (!ImGui::IsItemActive()) m_drag = -1;
    if (hovered && !m_pinned && m_drag < 0) { m_selX = cx; m_selY = cy; }
    if (m_selX < 0 || m_selY < 0 || m_selX >= m_cols || m_selY >= m_rows) { m_selX = m_cols / 2; m_selY = m_rows / 2; }
}

void RasterScene::DrawCells(ImDrawList* dl) const
{
    const bool cmpOk = CompareReady();
    if (m_view == VIEW_CPU)
        dl->AddRectFilled(ImVec2(m_gx0, m_gy0), ImVec2(m_gx0 + GridW(), m_gy0 + GridH()), IM_COL32(26, 26, 33, 255));
    for (int y = 0; y < m_rows; y++)
        for (int x = 0; x < m_cols; x++)
        {
            CellCalc cc = Evaluate(m_tri, P2{ x + 0.5, y + 0.5 }, m_cull, m_topLeft);
            ImVec2 a(m_gx0 + x * m_cell, m_gy0 + y * m_cell), b(a.x + m_cell, a.y + m_cell);
            ImVec2 ctr(a.x + m_cell * 0.5f, a.y + m_cell * 0.5f);
            if (m_view == VIEW_CPU && cc.inside)
                dl->AddRectFilled(a, b, IM_COL32(ToByte(cc.w[0]), ToByte(cc.w[1]), ToByte(cc.w[2]), 255));
            if (m_view == VIEW_COMPARE)
            {
                if (cc.inside) dl->AddCircleFilled(ctr, std::max(1.5f, m_cell * 0.12f), IM_COL32(255, 255, 255, 230));
                if (cmpOk && m_cmp.diff[(size_t)y * m_cols + x])
                    dl->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), IM_COL32(255, 0, 255, 255), 0, 0, std::max(2.0f, m_cell * 0.12f));
            }
            else if (m_cell >= 10) dl->AddCircleFilled(ctr, 1.5f, IM_COL32(150, 150, 160, 120));   // 픽셀 중심
        }
}

void RasterScene::DrawGridLines(ImDrawList* dl) const
{
    const float gw = GridW(), gh = GridH();
    if (m_cell >= 6)
    {
        for (int x = 0; x <= m_cols; x++) dl->AddLine(ImVec2(m_gx0 + x * m_cell, m_gy0), ImVec2(m_gx0 + x * m_cell, m_gy0 + gh), IM_COL32(255, 255, 255, 28));
        for (int y = 0; y <= m_rows; y++) dl->AddLine(ImVec2(m_gx0, m_gy0 + y * m_cell), ImVec2(m_gx0 + gw, m_gy0 + y * m_cell), IM_COL32(255, 255, 255, 28));
    }
    dl->AddRect(ImVec2(m_gx0, m_gy0), ImVec2(m_gx0 + gw, m_gy0 + gh), IM_COL32(255, 255, 255, 70));
}

// 고른 칸: 세 조각(PBC, APC, ABP)을 색으로 보여 준다
void RasterScene::DrawSelectedCell(ImDrawList* dl) const
{
    const ImVec2 sp = ToScreen(SelectedCenter());
    const ImVec2 sa = ToScreen(m_tri.v[0]), sb = ToScreen(m_tri.v[1]), sc = ToScreen(m_tri.v[2]);
    dl->AddTriangleFilled(sp, sb, sc, IM_COL32(255, 60, 60, 28));    // PBC -> A 의 몫
    dl->AddTriangleFilled(sa, sp, sc, IM_COL32(60, 255, 60, 28));    // APC -> B 의 몫
    dl->AddTriangleFilled(sa, sb, sp, IM_COL32(80, 120, 255, 28));   // ABP -> C 의 몫
    dl->AddLine(sp, sa, IM_COL32(255, 120, 120, 160), 1.0f);
    dl->AddLine(sp, sb, IM_COL32(120, 255, 120, 160), 1.0f);
    dl->AddLine(sp, sc, IM_COL32(120, 160, 255, 160), 1.0f);
    ImVec2 a(m_gx0 + m_selX * m_cell, m_gy0 + m_selY * m_cell);
    dl->AddRect(a, ImVec2(a.x + m_cell, a.y + m_cell), m_pinned ? IM_COL32(0, 230, 255, 255) : IM_COL32(255, 220, 0, 255), 0, 0, 2.0f);
    dl->AddCircleFilled(sp, 3.0f, IM_COL32(255, 220, 0, 255));
}

// 삼각형 모서리 + 감는 방향 화살표 (A -> B -> C)
void RasterScene::DrawTriangleEdges(ImDrawList* dl, bool culled) const
{
    const float fs = ImGui::GetFontSize();
    const ImU32 edgeCol = culled ? IM_COL32(150, 150, 150, 200) : IM_COL32(255, 255, 255, 230);
    for (int i = 0; i < 3; i++)
    {
        ImVec2 p0 = ToScreen(m_tri.v[i]), p1 = ToScreen(m_tri.v[(i + 1) % 3]);
        dl->AddLine(p0, p1, edgeCol, 2.0f);
        float dx = p1.x - p0.x, dy = p1.y - p0.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1) continue;
        dx /= len; dy /= len;
        ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        float s = fs * 0.45f;
        dl->AddTriangleFilled(ImVec2(mid.x + dx * s, mid.y + dy * s),
                              ImVec2(mid.x - dx * s - dy * s * 0.6f, mid.y - dy * s + dx * s * 0.6f),
                              ImVec2(mid.x - dx * s + dy * s * 0.6f, mid.y - dy * s - dx * s * 0.6f), edgeCol);
    }
}

void RasterScene::DrawVertices(ImDrawList* dl) const
{
    const float fs = ImGui::GetFontSize();
    const ImU32 vcol[3] = { IM_COL32(255, 70, 70, 255), IM_COL32(70, 230, 70, 255), IM_COL32(80, 130, 255, 255) };
    const char* vname[3] = { "A", "B", "C" };
    for (int i = 0; i < 3; i++)
    {
        const ImVec2 p = ToScreen(m_tri.v[i]);
        float r = fs * 0.45f;
        dl->AddCircleFilled(p, r, vcol[i]);
        dl->AddCircle(p, r, IM_COL32(255, 255, 255, 255), 0, (m_drag == i) ? 3.0f : 1.5f);
        char lab[64]; snprintf(lab, sizeof lab, "%s (%.2f, %.2f)", vname[i], m_tri.v[i].x, m_tri.v[i].y);
        dl->AddText(ImVec2(p.x + r + 3, p.y - fs * 0.5f), IM_COL32(255, 255, 255, 255), lab);
    }
}

void RasterScene::DrawStatusText(ImDrawList* dl, const Canvas& c, const CellCalc& whole) const
{
    const float fs = ImGui::GetFontSize();
    const char* viewName[3] = { "CPU 계산 (내가 직접 무게를 구해서 칠함)", "GPU 진짜 결과 (DX12 래스터라이저가 칠함)", "비교: 흰 점 = CPU 가 칠한 칸, 분홍 테두리 = CPU 와 GPU 가 다른 칸" };
    dl->AddText(ImVec2(m_gx0, c.y + fs * 0.4f), IM_COL32(140, 200, 255, 255), viewName[m_view]);
    char status[256];
    if (whole.area2 == 0.0) snprintf(status, sizeof status, "세 점이 한 줄 위에 있음 -> 넓이 0 -> 칠할 칸 없음");
    else if (whole.culled) snprintf(status, sizeof status, "%s 이라서 컬링됨 -> 칸을 계산하기도 전에 통째로 버림", whole.front ? "앞면(시계 방향)" : "뒷면(반시계 방향)");
    else snprintf(status, sizeof status, "%s · 꼭짓점을 끌어 보세요 · 칸을 누르면 그 칸 계산표가 고정돼요", whole.front ? "앞면(시계 방향)" : "뒷면(반시계 방향)");
    dl->AddText(ImVec2(m_gx0, c.y + fs * 1.7f), IM_COL32(220, 220, 220, 255), status);
}

// ---------------------------------------------------------------------
// GPU 명령 기록 (Samples 의 PopulateCommandList 처럼 단계별로)
// ---------------------------------------------------------------------
void RasterScene::OnRender(ID3D12GraphicsCommandList* cl, const Canvas&)
{
    if (m_sh.NeedsRebuild()) BuildPipelines();   // .hlsl 파일이 바뀌었으면 다시 컴파일 (핫 리로드)
    if (m_lowCols != m_cols || m_lowRows != m_rows) CreateLowRes(m_cols, m_rows);
    if (!m_blitPso) return;                       // 처음부터 컴파일 실패: GPU 쪽은 못 그림 (에러는 편집 창에)

    SetRootArguments(cl);          // 루트 시그니처 + 루트 상수(꼭짓점) + SRV 표
    RenderTriangleToLowRes(cl);    // 1패스 [래스터-④]
    CopyLowResToReadback(cl);      // 되읽기 [래스터-⑥]
    DrawLowResToScreen(cl);        // 2패스 [래스터-⑤]
}

TriConstants RasterScene::MakeConstants() const
{
    TriConstants k{};
    k.a[0] = (float)m_tri.v[0].x; k.a[1] = (float)m_tri.v[0].y;
    k.b[0] = (float)m_tri.v[1].x; k.b[1] = (float)m_tri.v[1].y;
    k.c[0] = (float)m_tri.v[2].x; k.c[1] = (float)m_tri.v[2].y;
    k.grid[0] = (float)m_cols; k.grid[1] = (float)m_rows;
    const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
    memcpy(k.colA, red, 16); memcpy(k.colB, green, 16); memcpy(k.colC, blue, 16);
    return k;
}

void RasterScene::SetRootArguments(ID3D12GraphicsCommandList* cl)
{
    const TriConstants k = MakeConstants();
    ID3D12DescriptorHeap* heaps[] = { g_dx.srvHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(m_rootSig.Get());
    cl->SetGraphicsRoot32BitConstants(0, sizeof(TriConstants) / 4, &k, 0);
    cl->SetGraphicsRootDescriptorTable(1, m_lowSrvGpu);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ===== [래스터-④] 1패스: 칸 수만큼의 작은 렌더 타깃에 진짜 삼각형 =====
void RasterScene::RenderTriangleToLowRes(ID3D12GraphicsCommandList* cl)
{
    D3D12_RESOURCE_BARRIER b = Transition(m_low.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, &b);
    const float transparent[4] = { 0, 0, 0, 0 };
    cl->ClearRenderTargetView(m_lowRtv, transparent, 0, nullptr);
    cl->OMSetRenderTargets(1, &m_lowRtv, FALSE, nullptr);
    D3D12_VIEWPORT lowVp{ 0, 0, (float)m_cols, (float)m_rows, 0, 1 };
    D3D12_RECT lowSc{ 0, 0, m_cols, m_rows };
    cl->RSSetViewports(1, &lowVp);
    cl->RSSetScissorRects(1, &lowSc);
    cl->SetPipelineState(m_triPso[m_cull].Get());
    cl->DrawInstanced(3, 1, 0, 0);             // 정점 3개 (SV_VertexID = 0, 1, 2)
}

// ===== [래스터-⑥] 되읽기: 작은 그림을 Readback 버퍼로 복사 (CPU 는 2프레임 뒤에 읽음) =====
void RasterScene::CopyLowResToReadback(ID3D12GraphicsCommandList* cl)
{
    Slot& s = m_slots[g_dx.frameIndex];
    D3D12_RESOURCE_BARRIER b = Transition(m_low.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = s.readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = s.fp;
    src.pResource = m_low.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    s.valid = true; s.tri = m_tri; s.cols = m_cols; s.rows = m_rows; s.cull = m_cull; s.topLeft = m_topLeft; s.caseId = m_caseId;
    b = Transition(m_low.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl->ResourceBarrier(1, &b);
}

// ===== [래스터-⑤] 2패스: 작은 그림을 격자 영역에 크게 늘려 그리기 =====
void RasterScene::DrawLowResToScreen(ID3D12GraphicsCommandList* cl)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferRtv();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (m_view == VIEW_CPU) return;   // CPU 계산 보기는 ImGui 가 칸을 칠한다
    D3D12_VIEWPORT vp{ m_gx0, m_gy0, GridW(), GridH(), 0, 1 };
    D3D12_RECT sc{ 0, 0, (LONG)g_dx.width, (LONG)g_dx.height };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetPipelineState(m_blitPso.Get());
    cl->DrawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------
// 오른쪽 패널
// ---------------------------------------------------------------------
static void Eli5(const char* title, const char* analogy, const char* meaning, const char* does, const char* why, const char* example)
{
    if (!ImGui::CollapsingHeader(title)) return;
    ImGui::Indent();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "비유");    ImGui::TextWrapped("%s", analogy);
    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "뜻");      ImGui::TextWrapped("%s", meaning);
    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "하는 일"); ImGui::TextWrapped("%s", does);
    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "왜 있나"); ImGui::TextWrapped("%s", why);
    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "예");       ImGui::TextWrapped("%s", example);
    ImGui::Unindent();
    ImGui::Spacing();
}

void RasterScene::OnPanel()
{
    ImGui::TextWrapped("큰 칸 하나 = 진짜 픽셀 하나. 꼭짓점 A(빨강) B(초록) C(파랑)를 끌어 보고, 칸에 마우스를 올려 아래 계산표를 보세요.");
    PanelSettings();
    PanelShader();
    const CellCalc cc = Evaluate(m_tri, SelectedCenter(), m_cull, m_topLeft);
    PanelWholeTriangle(cc);   // ① 삼각형 전체 넓이
    PanelCellTable(cc);       // ② 고른 칸 계산표
    PanelCellVerdict(cc);
    PanelCellColors(cc);
    PanelCompare();           // ③ CPU vs GPU
    PanelEli5();              // 쉽게 설명
    PanelCodeFlow();
}

void RasterScene::PanelSettings()
{
    ImGui::SeparatorText("설정");
    ImGui::TextUnformatted("보기"); ImGui::SameLine();
    ImGui::RadioButton("CPU 계산", &m_view, VIEW_CPU); ImGui::SameLine();
    ImGui::RadioButton("GPU 결과", &m_view, VIEW_GPU); ImGui::SameLine();
    ImGui::RadioButton("비교", &m_view, VIEW_COMPARE);
    int cols = m_cols;
    if (ImGui::SliderInt("가로 칸 수", &cols, kMinCols, kMaxCols)) SetCols(cols);
    const char* snaps[3] = { "자유 (1/256칸)", "0.5칸 (픽셀 중심/경계)", "1칸" };
    if (ImGui::Combo("꼭짓점 붙이기", &m_snap, snaps, 3)) { ClampAndSnap(); m_caseId++; }
    ImGui::TextUnformatted("컬링"); ImGui::SameLine();
    if (ImGui::RadioButton("없음", &m_cull, CULLK_NONE)) m_caseId++; ImGui::SameLine();
    if (ImGui::RadioButton("뒷면 (BACK)", &m_cull, CULLK_BACK)) m_caseId++; ImGui::SameLine();
    if (ImGui::RadioButton("앞면 (FRONT)", &m_cull, CULLK_FRONT)) m_caseId++;
    if (ImGui::Checkbox("탑-레프트 규칙 (CPU 계산에 적용)", &m_topLeft)) m_caseId++;
    if (ImGui::Button("B와 C 바꾸기 (감는 방향 뒤집기)")) { std::swap(m_tri.v[1], m_tri.v[2]); m_caseId++; }
    if (ImGui::Button("처음 모양")) ResetShape();
    ImGui::SameLine();
    if (ImGui::Button("모서리 실험 모양")) EdgeShape();
}

void RasterScene::PanelShader()
{
    ImGui::SeparatorText("셰이더 (raster.hlsl)");
    if (ImGui::Button(m_sh.editorOpen ? "셰이더 창 닫기 (F2)" : "셰이더 창 열기 (F2)")) m_sh.editorOpen = !m_sh.editorOpen;
    ImGui::SameLine();
    if (ImGui::Button("다시 컴파일 (F5)")) m_sh.RequestRebuild();
    if (!m_sh.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "컴파일 에러: 셰이더 창을 보세요");
    else ImGui::TextColored(ImVec4(0.55f, 1, 0.55f, 1), "컴파일 성공 (%d번째) · 고치고 저장하면 바로 반영", m_sh.buildCount);
    ImGui::TextDisabled("GPU 결과 보기로 바꿔야 셰이더를 고친 효과가 보여요");
}

void RasterScene::PanelWholeTriangle(const CellCalc& cc)
{
    const P2 A = m_tri.v[0], B = m_tri.v[1], C = m_tri.v[2];
    ImGui::SeparatorText("① 삼각형 전체 넓이 (외적)");
    ImGui::Text("AB = B - A = (%.2f, %.2f)", B.x - A.x, B.y - A.y);
    ImGui::Text("AC = C - A = (%.2f, %.2f)", C.x - A.x, C.y - A.y);
    ImGui::Text("외적 = ABx*ACy - ABy*ACx");
    ImGui::Text("     = (%.2f)*(%.2f) - (%.2f)*(%.2f) = %.3f", B.x - A.x, C.y - A.y, B.y - A.y, C.x - A.x, cc.area2);
    ImGui::Text("넓이 = |외적| / 2 = %.3f 칸", std::fabs(cc.area2) * 0.5);
    if (cc.area2 == 0.0) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "부호 0: 세 점이 한 줄 -> 칠할 칸 없음");
    else ImGui::TextColored(cc.front ? ImVec4(0.6f, 1, 0.6f, 1) : ImVec4(1, 0.6f, 0.4f, 1), "부호 %s -> 화면에서 %s -> %s",
                            cc.front ? "+" : "-", cc.front ? "시계 방향" : "반시계 방향", cc.front ? "앞면" : "뒷면");
    if (cc.culled && cc.area2 != 0.0) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "컬링 설정 때문에 통째로 버려짐 (칸 계산 안 함)");
}

void RasterScene::PanelCellTable(const CellCalc& cc)
{
    const P2 P = SelectedCenter();
    ImGui::SeparatorText("② 고른 칸 계산표");
    ImGui::Text("칸 (%d, %d) -> 픽셀 중심 P = (%.1f, %.1f)%s", m_selX, m_selY, P.x, P.y, m_pinned ? "  [고정됨]" : "");
    if (!ImGui::BeginTable("w", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("조각");
    ImGui::TableSetupColumn("외적 (넓이 x2)");
    ImGui::TableSetupColumn("무게 = 조각 / 전체");
    ImGui::TableHeadersRow();
    const char* names[3] = { "PBC (A의 맞은편)", "APC (B의 맞은편)", "ABP (C의 맞은편)" };
    const char* wn[3] = { "wa", "wb", "wc" };
    const ImVec4 colr[3] = { ImVec4(1, 0.45f, 0.45f, 1), ImVec4(0.45f, 1, 0.45f, 1), ImVec4(0.5f, 0.65f, 1, 1) };
    for (int i = 0; i < 3; i++)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(colr[i], "%s", names[i]);
        ImGui::TableNextColumn(); ImGui::Text("%.3f", cc.e[i]);
        ImGui::TableNextColumn();
        if (cc.w[i] < 0) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s = %.3f (음수!)", wn[i], cc.w[i]);
        else if (cc.onEdge[i]) ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s = 0 (모서리 위)", wn[i]);
        else ImGui::Text("%s = %.3f", wn[i], cc.w[i]);
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted("합");
    ImGui::TableNextColumn(); ImGui::Text("%.3f", cc.e[0] + cc.e[1] + cc.e[2]);
    ImGui::TableNextColumn(); ImGui::Text("%.3f", cc.w[0] + cc.w[1] + cc.w[2]);
    ImGui::EndTable();
}

void RasterScene::PanelCellVerdict(const CellCalc& cc)
{
    ImGui::TextDisabled("세 조각의 합 = 전체 외적 (%.3f), 무게의 합 = 1", cc.area2);
    if (cc.culled) { ImGui::TextWrapped("판정: 삼각형이 컬링돼서 이 칸도 안 칠함"); return; }
    if (cc.inside) { ImGui::TextColored(ImVec4(0.6f, 1, 0.6f, 1), "판정: 무게가 모두 0 이상 -> 안쪽 -> 칠함"); return; }
    bool edgeOnly = true;   // 음수 무게는 없고 0 만 있음 = 모서리 위
    for (int i = 0; i < 3; i++) { double s = cc.front ? cc.e[i] : -cc.e[i]; if (s < 0) edgeOnly = false; }
    if (edgeOnly) ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "판정: 모서리 위인데 위쪽/왼쪽 모서리가 아님 -> 탑-레프트 규칙으로 안 칠함");
    else ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "판정: 음수 무게가 있음 -> 바깥 -> 안 칠함");
}

static void ColorSwatch(const char* id, unsigned char r, unsigned char g, unsigned char b)
{
    ImGui::SameLine();
    ImGui::ColorButton(id, ImVec4(r / 255.f, g / 255.f, b / 255.f, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize() * 1.4f, ImGui::GetFontSize()));
}

// 고른 칸의 색: CPU 가 계산한 색과 GPU 가 실제로 칠한 색
void RasterScene::PanelCellColors(const CellCalc& cc)
{
    if (cc.inside)
    {
        unsigned char r = ToByte(cc.w[0]), g = ToByte(cc.w[1]), bl = ToByte(cc.w[2]);
        ImGui::Text("색 = wa*빨강 + wb*초록 + wc*파랑 = (%d, %d, %d)", r, g, bl);
        ColorSwatch("##cpucol", r, g, bl);
    }
    if (!CompareReady()) return;
    const unsigned char* g = &m_cmp.gpu[((size_t)m_selY * m_cols + m_selX) * 4];
    if (g[3] >= 128)
    {
        ImGui::Text("GPU 가 칠한 색 = (%d, %d, %d)", g[0], g[1], g[2]);
        ColorSwatch("##gpucol", g[0], g[1], g[2]);
    }
    else ImGui::TextUnformatted("GPU: 이 칸은 안 칠함");
}

void RasterScene::PanelCompare()
{
    ImGui::SeparatorText("③ CPU 계산 vs GPU 진짜 결과");
    if (!CompareReady()) { ImGui::TextDisabled("GPU 결과를 기다리는 중..."); return; }
    ImGui::Text("칠한 칸: CPU %d개 / GPU %d개", m_cmp.cpuCount, m_cmp.gpuCount);
    if (m_cmp.diffCount == 0) ImGui::TextColored(ImVec4(0.6f, 1, 0.6f, 1), "다른 칸 0개 -> GPU 도 똑같은 계산을 해요!");
    else ImGui::TextColored(ImVec4(1, 0.5f, 1, 1), "다른 칸 %d개 (비교 보기에서 분홍 테두리)", m_cmp.diffCount);
    ImGui::Text("같은 칸의 색 차이: 최대 %d / 255", m_cmp.maxColorDiff);
    if (m_cmp.diffCount > 0 && !m_topLeft) ImGui::TextWrapped("탑-레프트 규칙을 껐기 때문이에요. GPU 는 항상 이 규칙을 써요. 다시 켜 보세요.");
}

void RasterScene::PanelEli5()
{
    ImGui::SeparatorText("쉽게 설명 (눌러서 펼치기)");
    Eli5("래스터라이저 (Rasterizer)",
         "모눈종이에 삼각형을 그려 놓고, 칸마다 \"이 칸 한가운데 점이 삼각형 안에 있나?\"를 물어서 안이면 색칠하는 사람이에요.",
         "Raster = 격자(모눈). 꼭짓점 3개로 된 삼각형을 \"칠할 픽셀 목록\"으로 바꾸는 GPU 의 고정 단계예요. 정점 셰이더(VS) 다음, 픽셀 셰이더(PS) 앞에 있어요.",
         "칸마다 안/밖을 판정하고, 안쪽 칸이면 무게(wa, wb, wc)로 꼭짓점 값(색, UV 등)을 섞어서 PS 에 넘겨요.",
         "모니터는 픽셀 격자라서 매끈한 삼각형을 그대로 보여 줄 수 없어요. 결국 \"어느 칸을 칠할지\"로 바꿔야 해요.",
         "이 화면의 큰 칸 하나가 진짜 화면 픽셀 하나예요. \"GPU 결과\" 보기는 실제로 가로 칸 수만큼의 작은 그림에 GPU 가 그린 거예요.");
    Eli5("픽셀 중심 (+0.5)",
         "칸에 이름표를 붙일 때 칸의 모서리가 아니라 한가운데에 핀을 꽂는 거예요.",
         "픽셀 (x, y) 의 판정 점은 (x + 0.5, y + 0.5) 예요. D3D12 의 규칙이에요.",
         "안/밖 판정과 색 섞기는 모두 이 한 점에서만 해요. 칸의 나머지 부분은 상관없어요.",
         "모서리를 기준으로 하면 그림이 반 칸씩 한쪽으로 쏠려 보여요.",
         "칸 (3, 2) 의 중심은 (3.5, 2.5). 삼각형이 칸을 반 넘게 덮어도 중심을 안 덮으면 안 칠해요. 꼭짓점을 천천히 끌면서 칸이 \"딱\" 바뀌는 순간을 보세요.");
    Eli5("외적 = 넓이 (부호 = 방향)",
         "두 화살표 AB, AC 로 평행사변형을 만들면 그 넓이가 외적이에요. 삼각형은 그 절반이에요. 그리고 A->B->C 가 시계 방향인지 반시계 방향인지가 + / - 부호로 나와요.",
         "2D 외적 = ABx*ACy - ABy*ACx. 결과는 숫자 하나예요.",
         "넓이를 구하고, 동시에 감는 방향(앞면/뒷면)을 알려 줘요. 무게 계산과 컬링 둘 다 이걸 써요.",
         "삼각형 넓이를 루트나 각도 없이 곱하기 두 번, 빼기 한 번으로 구할 수 있어서 GPU 가 엄청 빨리 할 수 있어요.",
         "A(0,0) B(4,0) C(0,3): 외적 = 4*3 - 0*0 = 12 -> 넓이 6. 화면 좌표는 y 가 아래로 커져서, + 가 화면에서 시계 방향이에요.");
    Eli5("무게중심 좌표 (wa, wb, wc)",
         "세 친구 A, B, C 가 점 P 를 나눠 갖는다고 해요. P 가 A 에 가까울수록 A 몫이 커져요. A 의 몫 = A 맞은편 조각(PBC)의 넓이 / 전체 넓이.",
         "wa = 넓이(PBC)/넓이(ABC), wb = 넓이(APC)/넓이(ABC), wc = 넓이(ABP)/넓이(ABC). 셋을 더하면 항상 1.",
         "1) 안/밖 판정: 셋 다 0 이상이면 안쪽, 하나라도 음수면 바깥.  2) 섞기: 색 = wa*A색 + wb*B색 + wc*C색.",
         "꼭짓점에만 값(색, UV, 노멀)이 있는데 픽셀마다 값이 필요해서요. 무게로 섞으면 매끈하게 이어져요.",
         "A=빨강, B=초록, C=파랑이라 이 장면에서는 픽셀 색이 그대로 (wa, wb, wc) 예요. P 가 A 위면 (255, 0, 0), 가운데면 약 (85, 85, 85). HelloTriangle 의 COLOR 도 이렇게 섞여서 PS 로 들어와요.");
    Eli5("컬링 (Culling)",
         "종이 인형의 뒷면은 어차피 안 보이니까 칠하지 않는 거예요. 일이 절반으로 줄어요.",
         "삼각형이 화면에서 어느 방향으로 감기는지(외적 부호)로 앞면/뒷면을 정하고, 정한 쪽을 버려요. DX12 기본은 FrontCounterClockwise = FALSE, 즉 화면에서 시계 방향이 앞면이에요.",
         "칸 계산을 하기도 전에 삼각형을 통째로 버려요. 설정은 PSO 의 D3D12_RASTERIZER_DESC.CullMode (NONE / BACK / FRONT).",
         "큐브 같은 닫힌 물체는 뒤쪽 면이 앞쪽 면에 가려 안 보여요. 안 보일 걸 칠하는 건 낭비예요.",
         "컬링을 \"뒷면\"으로 두고 \"B와 C 바꾸기\"를 누르면 감는 방향이 반시계가 되어 삼각형이 사라져요. 다음 장면(큐브)에서 꼭 필요해요.");
    Eli5("탑-레프트 규칙 (Top-Left Rule)",
         "두 집 사이 담장 위에 공이 떨어지면 한 집만 가져가야 해요. 규칙: \"내 집의 위쪽 담장이나 왼쪽 담장이면 내 거\".",
         "픽셀 중심이 모서리 위에 정확히 걸치면(무게 = 0), 그 모서리가 삼각형의 위쪽 수평 모서리이거나 왼쪽 모서리일 때만 칠해요.",
         "모서리 위 칸을 딱 한 삼각형에게만 줘요.",
         "두 삼각형이 모서리를 같이 쓸 때(사각형 = 삼각형 2개) 같은 픽셀을 두 번 칠하거나(반투명이면 진해짐) 아무도 안 칠해서 틈이 생기는 걸 막아요.",
         "\"모서리 실험 모양\"을 누르고 비교 보기에서 규칙을 껐다 켜 보세요. 끄면 오른쪽 아래 빗변 위 칸들이 GPU 와 달라져요(분홍).");
    Eli5("GPU 좌표 반올림 (1/256 픽셀)",
         "자로 재는데 눈금이 1/256 칸까지만 있는 자예요. 그 사이 값은 가까운 눈금으로 맞춰요.",
         "GPU 는 꼭짓점 위치를 픽셀의 1/256 단위(서브픽셀 정밀도 8비트)로 반올림한 뒤 판정해요.",
         "아주 작은 차이로 결과가 들쭉날쭉하지 않게 고정된 정밀도로 계산해요.",
         "고정된 눈금이면 같은 입력에 항상 같은 결과가 나오고, 하드웨어가 정수로 빠르게 계산할 수 있어요.",
         "그래서 이 앱의 꼭짓점 좌표도 항상 1/256 칸 단위로 맞춰 둬요. 그래야 CPU 와 GPU 결과가 칸 하나까지 같아요.");
}

void RasterScene::PanelCodeFlow()
{
    if (!ImGui::CollapsingHeader("이 장면의 DX12 코드 흐름")) return;
    ImGui::Indent();
    ImGui::TextWrapped("1패스 [래스터-④] RenderTriangleToLowRes(): 칸 수만큼 작은 렌더 타깃(예: %d x %d 픽셀)에 진짜 삼각형을 그려요. 정점 버퍼가 없어요. 꼭짓점 좌표는 루트 상수(Root Constants, 20개)로 보내고, VS 가 SV_VertexID(0, 1, 2)로 골라 써요. 컬링은 PSO 3개 중 하나를 골라서 바꿔요.", m_cols, m_rows);
    ImGui::TextWrapped("2패스 [래스터-⑤] DrawLowResToScreen(): 그 작은 그림을 화면의 격자 영역에 크게 늘려 그려요. 샘플러를 POINT(가장 가까운 픽셀 하나)로 해서 칸이 흐려지지 않아요.");
    ImGui::TextWrapped("되읽기 [래스터-⑥] CopyLowResToReadback(): 작은 그림을 Readback 힙 버퍼(시스템 RAM)로 복사하고, 그 프레임 자원 세트가 다시 돌아오면(2프레임 뒤) OnFrameReused() 에서 CPU 가 읽어서 칸마다 CPU 계산과 비교해요.");
    ImGui::TextWrapped("CPU 계산 [래스터-①②③]: Evaluate() 함수 하나에 외적, 무게, 컬링, 탑-레프트 규칙이 다 있어요.");
    ImGui::Unindent();
}

// ---------------------------------------------------------------------
// 자동 테스트: 여러 모양 / 컬링 / 칸 수에서 CPU 와 GPU 가 칸 하나까지 같은지
// ---------------------------------------------------------------------
static const int kTestCases = 74;

void RasterScene::ApplyTestCase(int k)
{
    srand(1234 + k * 7);
    auto rnd = [](double lo, double hi) { return lo + (hi - lo) * (rand() / (double)RAND_MAX); };
    static const int colsList[4] = { 8, 17, 24, 40 };
    SetCols(colsList[k % 4]);
    m_cull = (k / 4) % 3;
    m_topLeft = true;
    m_snap = (k % 3 == 0) ? 0 : 1;   // 자유 좌표와 0.5칸 좌표 둘 다 (0.5칸이면 모서리 위 칸이 자주 생김)
    m_view = k % 3;                  // 보기 모드 3가지도 돌려 본다
    const bool edge = (k % 12 == 5) || k >= kTestCases - 2;
    if (edge)
    {
        // 모서리 위 칸이 많은 모양: 탑-레프트 규칙까지 GPU 와 같은지 확인 (컬링 없음)
        EdgeShape();
        m_cull = CULLK_NONE;
        if (k % 24 == 17) std::swap(m_tri.v[1], m_tri.v[2]);   // 반시계 방향에서도 확인
        if (k == kTestCases - 1) m_topLeft = false;            // 규칙을 끄면 반드시 달라져야 함
    }
    else
    {
        for (auto& v : m_tri.v) v = { rnd(0, m_cols), rnd(0, m_rows) };
        if (k % 2) std::swap(m_tri.v[1], m_tri.v[2]);
    }
    ClampAndSnap();
    m_caseId++;
}

bool RasterScene::AutoTest(int frame, int& failures)
{
    if (frame < 3) return false;          // 첫 배치가 정해질 때까지
    if (m_testCase < 0) { m_testCase = 0; ApplyTestCase(0); m_testWait = 0; return false; }
    m_testWait++;
    if (m_cmp.valid && m_cmp.caseId == m_caseId && m_cmp.cols == m_cols && m_cmp.rows == m_rows)
    {
        if (!CheckAndLogTestCase()) failures++;
        return NextTestCase();
    }
    if (m_testWait > 60)
    {
        failures++;
        FILE* f = fopen("lab_autotest.log", "a"); if (f) { fprintf(f, "raster case %d TIMEOUT\n", m_testCase); fclose(f); }
        return NextTestCase();
    }
    return false;
}

// 지금 케이스의 비교 결과를 판정하고 로그에 한 줄 남긴다. 통과면 true
bool RasterScene::CheckAndLogTestCase()
{
    bool expectDiff = !m_topLeft;   // 마지막 케이스: 규칙을 끈 CPU 는 GPU 와 달라야 정상
    bool fail = (expectDiff ? m_cmp.diffCount == 0 : m_cmp.diffCount != 0) || m_cmp.maxColorDiff > 2;
    FILE* f = fopen("lab_autotest.log", "a");
    if (f)
    {
        fprintf(f, "raster case %2d cols=%d rows=%d cull=%d snap=%d topLeft=%d A(%.3f,%.3f) B(%.3f,%.3f) C(%.3f,%.3f) cpu=%d gpu=%d diff=%d colorDiff=%d %s\n",
                m_testCase, m_cols, m_rows, m_cull, m_snap, (int)m_topLeft, m_tri.v[0].x, m_tri.v[0].y, m_tri.v[1].x, m_tri.v[1].y, m_tri.v[2].x, m_tri.v[2].y,
                m_cmp.cpuCount, m_cmp.gpuCount, m_cmp.diffCount, m_cmp.maxColorDiff, fail ? "FAIL" : "ok");
        fclose(f);
    }
    return !fail;
}

// 다음 케이스로. 다 했으면 true
bool RasterScene::NextTestCase()
{
    if (++m_testCase >= kTestCases) return true;
    ApplyTestCase(m_testCase);
    m_testWait = 0;
    return false;
}

} // namespace

Scene* CreateRasterScene() { return new RasterScene(); }
