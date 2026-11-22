// =====================================================================
//  장면 3: 돌아가는 삼각형 - 상수를 GPU 에 보내는 세 가지 길 (학습 로드맵 2단계)
//
//  삼각형 3개를 그리고, 삼각형마다 "회전, 위치, 크기, 색" 12개 숫자를 GPU 에 보낸다.
//  같은 숫자를 세 가지 길로 보낼 수 있다. 오른쪽 패널에서 바꿔 가며 비교한다.
//
//    방법 A  루트 상수          : 숫자를 가방(루트 시그니처)에 직접 넣는다     = 쪽지에 숫자를 직접 적기
//    방법 B  루트 CBV           : 숫자는 Upload 버퍼에 두고, 가방엔 주소만    = 사물함 주소를 적기
//    방법 C  디스크립터 테이블  : 가방엔 "주소록 몇 번째 칸"만               = 주소록 칸 번호를 적기
//
//  셰이더(lab/shaders/spin.hlsl)는 세 방법이 모두 같은 파일을 쓴다. 셰이더는 "b0 에 값이 있다"만 안다.
//  일부러 틀리기: "상수 버퍼 한 칸만 쓰기"를 켜면 B, C 에서 삼각형 3개가 1개로 겹친다 (이유는 패널 설명).
//
//  코드에서 Ctrl+F 로 찾기:
//    [회전-①] 보낼 값 12개 (C++ 구조체 = HLSL cbuffer)
//    [회전-②] 루트 시그니처 3개: 가방 모양만 다르고 셰이더는 같다
//    [회전-③] Upload 버퍼: 256바이트 칸 (프레임 2개 x 삼각형 3개)
//    [회전-④] 디스크립터: 방법 C 의 "주소록" 칸에 Upload 버퍼 칸 주소 적기
//    [회전-⑤] 매 프레임 값 계산 (각도 -> cos, sin)
//    [회전-⑥] 그리기: 방법마다 값을 넘기는 함수가 다르다
//    [회전-⑦] 정점 버퍼 (게임 방식): 삼각형 모양 12바이트 x 3 을 Default 힙(VRAM)에 한 번 올려 두고 3번 같이 쓴다
//             비교용으로 "정점 버퍼 없이 (SV_VertexID)" 체크 상자를 켜면 장면 2처럼 셰이더 안의 점을 쓴다
// =====================================================================
#include "../scene.h"
#include "../mesh.h"
#include "../shader_file.h"
#include "../ui_help.h"
#include "imgui.h"
#include <d3d11shader.h>   // D3DReflect: 컴파일된 셰이더가 b0 에서 몇 바이트 읽는지 알아보기
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>

namespace {

// ===== [회전-①] 보낼 값 12개. HLSL 의 cbuffer SpinConstants 와 순서·크기가 같아야 한다 =====
struct SpinConstants
{
    float cosSin[2];   // 회전 (cos, sin)
    float offset[2];   // 중심 위치 (-1..1)
    float scale[2];    // 크기 (x 는 화면 비율 보정)
    float pad[2];      // 빈칸 (HLSL 의 16바이트 줄 맞추기)
    float color[4];
};
static_assert(sizeof(SpinConstants) == 48, "float 12개 = 48바이트");

// ===== [회전-⑦] 삼각형 모양 = 정점 버퍼. 정점 하나 12바이트, HLSL 의 SpinVertex 와 짝 =====
struct SpinVertex
{
    float position[2];   // POSITION : 정삼각형 꼭짓점 (무게중심 0, 0)
    float shade;         // SHADE    : 밝기 (0번 "머리" 1.0, 나머지 0.55)
};
static_assert(sizeof(SpinVertex) == 12, "float 3개 = 12바이트");
static const SpinVertex kTriangle[3] = { { { 0.0f, 1.0f }, 1.0f }, { { 0.866f, -0.5f }, 0.55f }, { { -0.866f, -0.5f }, 0.55f } };
static const D3D12_INPUT_ELEMENT_DESC kSpinLayout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SpinVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "SHADE",    0, DXGI_FORMAT_R32_FLOAT,    0, offsetof(SpinVertex, shade),    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

static const UINT kDraws    = 3;     // 삼각형 수
static const UINT kSlotSize = 256;   // 상수 버퍼 한 칸 = 256바이트 (D3D12 규칙: CBV 는 256바이트 단위)
static const UINT kSlots    = kFrameCount * kDraws;   // 프레임 2개 x 삼각형 3개 = 6칸

enum Method { kRootConst = 0, kRootCbv = 1, kTable = 2, kMethodCount = 3 };
static const char* kMethodName[kMethodCount] = { "A 루트 상수", "B 루트 CBV", "C 디스크립터 테이블" };
static const char* kMethodShort[kMethodCount] = { "쪽지에 숫자를 직접 적기", "사물함 주소를 적기", "주소록 칸 번호를 적기" };

// 삼각형마다 고정 값: 위치, 회전 빠르기, 색
static const float kOffsetX[kDraws] = { -0.55f, 0.0f, 0.55f };
static const float kSpin[kDraws]    = { 1.0f, -0.7f, 1.6f };   // 초당 라디안 (음수 = 반대로)
static const float kColor[kDraws][4] = { { 1.0f, 0.35f, 0.3f, 1 }, { 0.3f, 0.9f, 0.4f, 1 }, { 0.35f, 0.55f, 1.0f, 1 } };

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

class SpinScene : public Scene
{
public:
    const char* Name() const override { return "장면 3: 돌아가는 삼각형 (상수를 보내는 세 가지 길)"; }
    void Init() override;
    void Shutdown() override;
    void OnFrameReused(UINT slot) override;
    void OnPanel() override;
    void OnCanvas(const Canvas& c) override;
    void OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c) override;
    void OnMinimized(const Canvas& c) override;
    bool AutoTest(int frame, int& failures) override;

private:
    ShaderFile                  m_sh;   // lab/shaders/spin.hlsl
    ComPtr<ID3D12RootSignature> m_rootSig[kMethodCount];
    ComPtr<ID3D12PipelineState> m_pso[kMethodCount];        // 게임 방식: 정점 버퍼 (SpinVS)
    ComPtr<ID3D12PipelineState> m_psoNoBuf[kMethodCount];   // 비교용: 정점 버퍼 없이 (SpinVSNoBuffer)
    Mesh                        m_mesh;                     // [회전-⑦] 삼각형 모양 (Default 힙)
    bool                        m_noBuffer = false;         // 비교용 체크 상자
    ID3D12PipelineState* Pso(int m) const { return m_noBuffer ? m_psoNoBuf[m].Get() : m_pso[m].Get(); }
    void DrawTriangle(ID3D12GraphicsCommandList* cl) const;
    std::string                 m_psoError[kMethodCount];
    // 셰이더가 b0 에서 실제로 읽는 바이트 (리플렉션으로 알아냄, 0 = 알 수 없음)
    // PSO 를 만들 때 D3D12 는 "루트 상수 개수 < 셰이더가 읽는 크기"를 막아 주지 않아요. 그래서 직접 검사해요.
    UINT                        m_b0Used = 0;
    UINT                        m_b0Declared = 0;

    // 방법 B, C 가 쓰는 Upload 버퍼 (CPU 가 쓰고 GPU 가 읽는 RAM)
    ComPtr<ID3D12Resource>      m_upload;
    uint8_t*                    m_mapped = nullptr;   // CPU 가 쓰는 주소 (계속 Map 해 둠)
    D3D12_CPU_DESCRIPTOR_HANDLE m_cbvCpu[kSlots] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_cbvGpu[kSlots] = {};
    UINT                        m_cbvIndex[kSlots] = {};   // 디스크립터 힙에서 몇 번째 칸인지 (패널 표시용)

    // 설정
    int    m_method = kRootConst;
    bool   m_oneSlot = false;   // 일부러 틀리기: 삼각형 3개가 상수 버퍼 한 칸을 같이 씀
    bool   m_paused = false;
    float  m_speed = 1.0f;
    double m_time = 0.0;
    float  m_aspect = 1.0f;     // 그림 영역 높이 / 너비

    SpinConstants m_last[kDraws] = {};   // 마지막으로 보낸 값 (패널 표시용)
    UINT          m_lastSlot[kDraws] = {};

    void CreateRootSigs();
    void CreateUploadBuffer();
    void CreateCbvDescriptors();
    bool BuildPipelines();
    bool BuildPso(int method, ID3DBlob* vs, ID3DBlob* vsNoBuf, ID3DBlob* ps);
    void PanelB0Check();
    void MakeConstants(SpinConstants out[kDraws]) const;
    UINT SlotFor(UINT draw) const;
    void WriteSlot(UINT slot, const SpinConstants& k);
    void DrawWithRootConstants(ID3D12GraphicsCommandList* cl, const SpinConstants k[kDraws]);
    void DrawWithRootCbv(ID3D12GraphicsCommandList* cl, const SpinConstants k[kDraws]);
    void DrawWithTable(ID3D12GraphicsCommandList* cl, const SpinConstants k[kDraws]);
    void DrawCanvasText(const Canvas& c);

    void PanelMethod();
    void PanelMotion();
    void PanelShader();
    void PanelBag();
    void PanelValues();
    void PanelSlots();
    void PanelEli5();
    void PanelCodeFlow();

    // ---- 개발용 자동 테스트 (화면의 삼각형 중심 픽셀을 되읽어 색을 확인) ----
    ComPtr<ID3D12Resource> m_readback[kFrameCount];
    int  m_capTag[kFrameCount] = { -1, -1 };
    int  m_testCase = -1, m_testWait = 0;
    int  m_gotTag = -1;
    int  m_gotClass[kDraws] = {};
    unsigned m_gotRgb[kDraws][3] = {};
    void CaptureCenters(ID3D12GraphicsCommandList* cl, const Canvas& c);
    void ApplyTestCase(int k);
};

// ---------------------------------------------------------------------
// GPU 자원 (Samples 의 LoadAssets)
// ---------------------------------------------------------------------
void SpinScene::Init()
{
    CreateRootSigs();        // [회전-②]
    CreateUploadBuffer();    // [회전-③]
    CreateCbvDescriptors();  // [회전-④]
    // [회전-⑦] 삼각형 모양을 VRAM 에 한 번 올린다 (인덱스 없이 점 3개. 안은 mesh.cpp 의 [정점-②~⑥])
    m_mesh = CreateMesh(kTriangle, sizeof(SpinVertex), 3, nullptr, 0, MeshHeap::Default, L"spin triangle");
    m_sh.Open("spin.hlsl");
    m_sh.editorOpen = false;   // 이 장면의 주인공은 패널(세 가지 길 비교). 셰이더 창은 F2 로
    BuildPipelines();
    if (GetEnvironmentVariableA("LAB_AUTOTEST", nullptr, 0) > 0)
        for (UINT f = 0; f < kFrameCount; f++)
            m_readback[f] = CreateBuffer(D3D12_HEAP_TYPE_READBACK, 512 * kDraws, D3D12_RESOURCE_STATE_COPY_DEST);
}

void SpinScene::Shutdown()
{
    for (UINT i = 0; i < kSlots; i++) if (m_cbvCpu[i].ptr) FreeSrv(m_cbvCpu[i]);
    if (m_upload && m_mapped) { m_upload->Unmap(0, nullptr); m_mapped = nullptr; }
}

// ===== [회전-②] 루트 시그니처 3개: 가방 칸이 b0 하나뿐인 건 같고, 그 칸에 무엇을 넣는지만 다르다 =====
void SpinScene::CreateRootSigs()
{
    // A: 숫자 12개를 가방에 직접 (가방 64칸 중 12칸)
    D3D12_ROOT_PARAMETER a{};
    a.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    a.Constants.ShaderRegister = 0;
    a.Constants.Num32BitValues = sizeof(SpinConstants) / 4;
    a.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // B: 상수 버퍼의 GPU 주소 하나 (64비트 주소라 2칸)
    D3D12_ROOT_PARAMETER b{};
    b.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    b.Descriptor.ShaderRegister = 0;
    b.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // C: 디스크립터 힙(주소록)의 칸 번호 하나 (1칸). 그 칸에 CBV(주소 + 크기)가 적혀 있다
    static D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER c{};
    c.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    c.DescriptorTable.NumDescriptorRanges = 1;
    c.DescriptorTable.pDescriptorRanges = &range;
    c.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const D3D12_ROOT_PARAMETER params[kMethodCount] = { a, b, c };
    for (int m = 0; m < kMethodCount; m++)
    {
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 1; rs.pParameters = &params[m];
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;   // 정점 버퍼(입력 레이아웃)를 쓴다
        m_rootSig[m] = CreateRootSignature(rs);
    }
}

// ===== [회전-③] Upload 버퍼: 256바이트 칸 6개 = 프레임 2개 x 삼각형 3개 =====
// Upload 힙 = 발표의 "CPU 가 쓰고 GPU 가 읽는 RAM". 방법 B, C 의 숫자는 여기에 산다.
void SpinScene::CreateUploadBuffer()
{
    m_upload = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, (uint64_t)kSlotSize * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RANGE none{ 0, 0 };   // CPU 는 읽지 않음 (쓰기만)
    HR(m_upload->Map(0, &none, (void**)&m_mapped));   // 한 번 Map 해 두고 계속 쓴다 (Samples 와 같음)
    memset(m_mapped, 0, (size_t)kSlotSize * kSlots);
}

// ===== [회전-④] 디스크립터: 주소록(CBV/SRV/UAV 디스크립터 힙) 칸마다 "Upload 버퍼 몇 번째 칸, 256바이트"를 적는다 =====
void SpinScene::CreateCbvDescriptors()
{
    const D3D12_GPU_VIRTUAL_ADDRESS base = m_upload->GetGPUVirtualAddress();
    const SIZE_T heapStart = g_dx.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    for (UINT i = 0; i < kSlots; i++)
    {
        AllocSrv(&m_cbvCpu[i], &m_cbvGpu[i]);
        m_cbvIndex[i] = (UINT)((m_cbvCpu[i].ptr - heapStart) / g_dx.srvInc);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        cbv.BufferLocation = base + (uint64_t)i * kSlotSize;   // 256 의 배수여야 한다
        cbv.SizeInBytes = kSlotSize;                            // 이것도 256 의 배수
        g_dx.device->CreateConstantBufferView(&cbv, m_cbvCpu[i]);
    }
}

// 컴파일된 셰이더에게 "b0 에서 몇 바이트를 읽니?" 물어보기 (셰이더 리플렉션)
// cbuffer 에 적혀만 있고 아무도 안 읽는 값은 D3D_SVF_USED 가 꺼져 있어요 -> 세지 않아요.
// declared 에는 cbuffer 에 적힌 전체 크기(16바이트 줄 단위로 올림)를 넣어요.
static UINT B0BytesRead(ID3DBlob* blob, UINT* declared)
{
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D11ShaderReflection, (void**)refl.GetAddressOf())))
        return 0;
    D3D11_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) return 0;
    for (UINT r = 0; r < sd.BoundResources; r++)
    {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (FAILED(refl->GetResourceBindingDesc(r, &bd))) continue;
        if (bd.Type != D3D_SIT_CBUFFER || bd.BindPoint != 0) continue;
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName(bd.Name);
        D3D11_SHADER_BUFFER_DESC cd{};
        if (!cb || FAILED(cb->GetDesc(&cd))) return 0;
        *declared = std::max(*declared, (UINT)cd.Size);
        UINT used = 0;
        for (UINT v = 0; v < cd.Variables; v++)
        {
            D3D11_SHADER_VARIABLE_DESC vd{};
            if (FAILED(cb->GetVariableByIndex(v)->GetDesc(&vd))) continue;
            if (vd.uFlags & D3D_SVF_USED) used = std::max(used, vd.StartOffset + vd.Size);
        }
        return used;
    }
    return 0;
}

// 셰이더는 한 번 컴파일해서 PSO 3개(루트 시그니처마다 하나)에 같이 쓴다
bool SpinScene::BuildPipelines()
{
    ComPtr<ID3DBlob> vs, vsNoBuf, ps;
    std::string err;
    bool ok = TryCompile(m_sh, "SpinVS", "vs_5_0", vs, err);
    ok &= TryCompile(m_sh, "SpinVSNoBuffer", "vs_5_0", vsNoBuf, err);
    ok &= TryCompile(m_sh, "SpinPS", "ps_5_0", ps, err);
    if (!ok) { m_sh.error = err; return false; }   // 문법 에러: 지금 쓰던 PSO 를 그대로 둔다
    m_b0Declared = 0;
    m_b0Used = std::max({ B0BytesRead(vs.Get(), &m_b0Declared), B0BytesRead(vsNoBuf.Get(), &m_b0Declared), B0BytesRead(ps.Get(), &m_b0Declared) });

    if (m_pso[0] || m_pso[1] || m_pso[2]) WaitIdle();   // GPU 가 옛 PSO 를 다 쓴 뒤에 바꾼다
    std::string all;
    for (int m = 0; m < kMethodCount; m++)
        if (!BuildPso(m, vs.Get(), vsNoBuf.Get(), ps.Get())) all += m_psoError[m];
    m_sh.error = all;
    m_sh.buildCount++;
    return all.empty();
}

// PSO 2개: 게임 방식(입력 레이아웃 있음) + 비교용(입력 레이아웃 없음). 다른 건 VS 와 InputLayout 뿐
bool SpinScene::BuildPso(int m, ID3DBlob* vs, ID3DBlob* vsNoBuf, ID3DBlob* ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_rootSig[m].Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { kSpinLayout, (UINT)(sizeof(kSpinLayout) / sizeof(kSpinLayout[0])) };   // [회전-⑦] 정점 버퍼 12바이트
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = kBackBufferFormat;
    pd.SampleDesc.Count = 1;
    m_pso[m].Reset();
    m_psoNoBuf[m].Reset();
    m_psoError[m].clear();
    const bool okGame = SUCCEEDED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso[m])));
    // 비교용: 정점 버퍼 없음 (SV_VertexID)
    pd.VS = { vsNoBuf->GetBufferPointer(), vsNoBuf->GetBufferSize() };
    pd.InputLayout = { nullptr, 0 };
    const bool okNoBuf = SUCCEEDED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoNoBuf[m])));
    if (okGame && okNoBuf) return true;
    char msg[512];
    snprintf(msg, sizeof msg,
             "방법 %s: 컴파일은 됐지만 PSO 를 못 만들었어요.\n"
             "%s",
             kMethodName[m],
             "흔한 이유: b1, t0 처럼 루트 시그니처(가방)에 없는 레지스터를 씀. 또는 SpinVertex 의 시맨틱(POSITION, SHADE)이 C++ 입력 레이아웃과 다름.\n");
    m_psoError[m] = msg;
    return false;
}

// ---------------------------------------------------------------------
// 왼쪽 그림 영역
// ---------------------------------------------------------------------
void SpinScene::OnCanvas(const Canvas& c)
{
    if (!m_paused) m_time += ImGui::GetIO().DeltaTime * m_speed;
    m_aspect = c.h / std::max(1.0f, c.w);
    DrawCanvasText(c);
    DrawShaderEditor(m_sh, c);
}

void SpinScene::DrawCanvasText(const Canvas& c)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    const float fs = ImGui::GetFontSize();
    char line[256];
    snprintf(line, sizeof line, "지금: 방법 %s (%s)%s · F2 셰이더 창", kMethodName[m_method], kMethodShort[m_method],
             m_oneSlot ? " · 일부러 틀리기 켜짐" : "");
    dl->AddText(ImVec2(c.x + fs * 0.6f, c.y + fs * 0.4f), IM_COL32(255, 255, 255, 210), line);
    if (!Pso(m_method))
        dl->AddText(ImVec2(c.x + fs * 0.6f, c.y + fs * 1.8f), IM_COL32(255, 120, 120, 255),
                    "이 방법의 PSO 가 없어서 아무것도 안 그려요. 셰이더 창의 빨간 글씨를 보세요.");
    if (m_method == kRootConst && m_b0Used > sizeof(SpinConstants))
        dl->AddText(ImVec2(c.x + fs * 0.6f, c.y + fs * 3.2f), IM_COL32(255, 120, 120, 255),
                    "주의: 셰이더가 가방(48바이트)보다 많이 읽어요. 넘친 자리는 정해지지 않은 값이에요.");
    if (m_oneSlot && m_method != kRootConst)
        dl->AddText(ImVec2(c.x + fs * 0.6f, c.y + fs * 1.8f), IM_COL32(255, 210, 120, 255),
                    "삼각형을 3번 그렸는데 1개만 보여요. 3개가 같은 칸의 마지막 값(파랑)을 읽었어요.");
}

void SpinScene::OnMinimized(const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipelines();
    DrawShaderEditor(m_sh, c);
}

// ---------------------------------------------------------------------
// GPU 명령 기록 (Samples 의 PopulateCommandList)
// ---------------------------------------------------------------------

// ===== [회전-⑤] 매 프레임 값 계산: 각도 -> cos, sin 은 CPU 가 한 번만 계산해서 보낸다 =====
void SpinScene::MakeConstants(SpinConstants out[kDraws]) const
{
    const float size = 0.3f;
    for (UINT i = 0; i < kDraws; i++)
    {
        const float angle = (float)m_time * kSpin[i] + 0.6f * i;
        SpinConstants& k = out[i];
        k = {};
        k.cosSin[0] = std::cos(angle); k.cosSin[1] = std::sin(angle);
        k.offset[0] = kOffsetX[i];     k.offset[1] = 0.0f;
        k.scale[0]  = size * m_aspect; k.scale[1]  = size;
        for (int j = 0; j < 4; j++) k.color[j] = kColor[i][j];
    }
}

// 삼각형 i 가 쓸 Upload 버퍼 칸 번호. 프레임 자원 세트마다 3칸씩 따로 쓴다.
// 일부러 틀리기를 켜면 3개 모두 그 프레임의 첫 칸을 같이 쓴다.
UINT SpinScene::SlotFor(UINT draw) const
{
    return g_dx.frameIndex * kDraws + (m_oneSlot ? 0 : draw);
}

// CPU 가 Upload 버퍼 칸에 48바이트를 쓴다 (나머지 208바이트는 빈칸)
void SpinScene::WriteSlot(UINT slot, const SpinConstants& k)
{
    memcpy(m_mapped + (size_t)slot * kSlotSize, &k, sizeof k);
}

// ===== [회전-⑥] 그리기: 삼각형마다 값을 넘기고 DrawInstanced(3) =====
void SpinScene::OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipelines();   // .hlsl 을 고쳤으면 다시 컴파일 (핫 리로드)

    SpinConstants k[kDraws];
    MakeConstants(k);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferRtv();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, c.w, c.h, 0, 1 };   // 백 버퍼 (0, 0) = 그림 영역 왼쪽 위
    D3D12_RECT sc{ 0, 0, (LONG)c.w, (LONG)c.h };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (Pso(m_method))
    {
        if (m_method == kRootConst) DrawWithRootConstants(cl, k);
        if (m_method == kRootCbv)   DrawWithRootCbv(cl, k);
        if (m_method == kTable)     DrawWithTable(cl, k);
    }
    for (UINT i = 0; i < kDraws; i++) m_last[i] = k[i];

    if (m_readback[0]) CaptureCenters(cl, c);   // 자동 테스트 때만
}

// [회전-⑦] 삼각형 하나 그리기: 게임 방식은 정점 버퍼를 꽂고 그린다. 비교용은 번호만 (SV_VertexID)
void SpinScene::DrawTriangle(ID3D12GraphicsCommandList* cl) const
{
    if (m_noBuffer) cl->DrawInstanced(3, 1, 0, 0);
    else            m_mesh.Draw(cl);   // IASetVertexBuffers -> DrawInstanced(3) (mesh.cpp)
}

// 방법 A: 숫자 12개를 명령 목록 안에 바로 복사한다. 그래서 삼각형마다 다른 값이 그대로 남는다
void SpinScene::DrawWithRootConstants(ID3D12GraphicsCommandList* cl, const SpinConstants k[kDraws])
{
    cl->SetGraphicsRootSignature(m_rootSig[kRootConst].Get());
    cl->SetPipelineState(Pso(kRootConst));
    for (UINT i = 0; i < kDraws; i++)
    {
        cl->SetGraphicsRoot32BitConstants(0, sizeof(SpinConstants) / 4, &k[i], 0);
        DrawTriangle(cl);
        m_lastSlot[i] = UINT_MAX;   // Upload 버퍼를 안 씀
    }
}

// 방법 B: 숫자는 Upload 버퍼 칸에 쓰고, 명령 목록에는 그 칸의 GPU 주소만 넣는다
void SpinScene::DrawWithRootCbv(ID3D12GraphicsCommandList* cl, const SpinConstants k[kDraws])
{
    cl->SetGraphicsRootSignature(m_rootSig[kRootCbv].Get());
    cl->SetPipelineState(Pso(kRootCbv));
    const D3D12_GPU_VIRTUAL_ADDRESS base = m_upload->GetGPUVirtualAddress();
    for (UINT i = 0; i < kDraws; i++)
    {
        const UINT slot = SlotFor(i);
        WriteSlot(slot, k[i]);
        cl->SetGraphicsRootConstantBufferView(0, base + (uint64_t)slot * kSlotSize);
        DrawTriangle(cl);
        m_lastSlot[i] = slot;
    }
}

// 방법 C: 숫자는 Upload 버퍼 칸에, 그 칸의 주소는 주소록(디스크립터 힙) 칸에, 명령 목록에는 주소록 칸 번호만
void SpinScene::DrawWithTable(ID3D12GraphicsCommandList* cl, const SpinConstants k[kDraws])
{
    ID3D12DescriptorHeap* heaps[] = { g_dx.srvHeap.Get() };   // "이 주소록을 보세요" (테이블 전에 꼭)
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(m_rootSig[kTable].Get());
    cl->SetPipelineState(Pso(kTable));
    for (UINT i = 0; i < kDraws; i++)
    {
        const UINT slot = SlotFor(i);
        WriteSlot(slot, k[i]);
        cl->SetGraphicsRootDescriptorTable(0, m_cbvGpu[slot]);
        DrawTriangle(cl);
        m_lastSlot[i] = slot;
    }
}

// ---------------------------------------------------------------------
// 오른쪽 패널
// ---------------------------------------------------------------------
void SpinScene::OnPanel()
{
    ImGui::TextWrapped("삼각형 3개에 \"회전, 위치, 크기, 색\" 숫자 12개씩을 보내요. 같은 숫자를 세 가지 길로 보낼 수 있어요. "
                       "길을 바꿔도 화면은 똑같아야 정상이에요. 다른 건 숫자가 어디에 살고 GPU 가 어떻게 찾아가는지예요.");
    PanelMethod();
    PanelMotion();
    PanelShader();
    PanelBag();
    PanelValues();
    PanelSlots();
    PanelEli5();
    PanelCodeFlow();
}

void SpinScene::PanelMethod()
{
    ImGui::SeparatorText("보내는 길");
    ImGui::RadioButton("A 루트 상수", &m_method, kRootConst);
    Help("쪽지에 숫자를 직접 적어 주기.\n"
         "숫자 12개를 명령 목록(GPU 에게 줄 주문서) 안에 그대로 복사해 넣어요. 따로 메모리를 만들 필요가 없어요.\n"
         "가장 빠르고 간단하지만, 가방(루트 시그니처)이 64칸뿐이라 숫자가 많으면 못 써요. 이 장면은 12칸을 써요.");
    ImGui::RadioButton("B 루트 CBV", &m_method, kRootCbv);
    Help("사물함 주소를 적어 주기.\n"
         "숫자는 Upload 버퍼(발표의 Upload 힙, CPU 가 쓰고 GPU 가 읽는 RAM)의 256바이트 칸에 쓰고, "
         "주문서에는 그 칸의 GPU 주소만 넣어요 (가방 2칸).\n"
         "숫자가 많아도 돼요 (한 칸 최대 64KB). 대신 CPU 가 쓴 칸을 GPU 가 다 읽기 전에 덮어쓰면 안 돼요.");
    ImGui::RadioButton("C 디스크립터 테이블", &m_method, kTable);
    Help("주소록 칸 번호를 적어 주기.\n"
         "숫자는 B 처럼 Upload 버퍼 칸에 있어요. 다른 점은, 그 칸의 주소와 크기를 \"디스크립터\"라는 카드에 적어서 "
         "주소록(디스크립터 힙)에 꽂아 두고, 주문서에는 주소록의 칸 번호만 넣는다는 거예요 (가방 1칸).\n"
         "한 번 더 찾아가야 해서 가장 돌아가는 길이지만, 칸 번호 하나로 카드 여러 장(텍스처 여러 개 등)을 한꺼번에 넘길 수 있어요. "
         "텍스처는 이 길로만 넘길 수 있어서 로드맵 3단계에서 다시 나와요.");

    ImGui::Spacing();
    ImGui::Checkbox("일부러 틀리기: 상수 버퍼 한 칸만 쓰기", &m_oneSlot);
    Help("켜면 삼각형 3개가 Upload 버퍼의 같은 칸 하나를 같이 써요.\n"
         "B, C 에서: 삼각형이 1개(파랑)만 보여요. 왜냐하면 CPU 는 주문서를 쓰는 동안 그 칸에 빨강 -> 초록 -> 파랑 값을 차례로 덮어쓰고, "
         "GPU 는 주문서를 다 받은 \"나중에\" 그리기 시작하거든요. 그때 칸에는 마지막 값(파랑)만 남아 있어서 3번 모두 파랑 자리에 그려요.\n"
         "A 에서: 아무 일도 없어요. 숫자가 주문서 안에 복사돼서 삼각형마다 따로 남으니까요.\n"
         "그래서 B, C 는 그리기마다, 그리고 프레임 자원 세트마다 칸을 따로 둬요 (아래 \"Upload 버퍼 칸\" 표).");
}

void SpinScene::PanelMotion()
{
    ImGui::SeparatorText("회전");
    if (ImGui::Button(m_paused ? "재생" : "멈춤")) m_paused = !m_paused;
    ImGui::SameLine();
    if (ImGui::Button("0초로")) m_time = 0.0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::SliderFloat("빠르기", &m_speed, 0.0f, 4.0f, "%.2f배");
    Help("각도는 시간으로 정해요. CPU 가 매 프레임 각도로 cos, sin 을 계산해서 보내고, 셰이더는 그걸로 꼭짓점을 돌려요.\n"
         "멈추면 보내는 값이 그대로라 아래 \"지금 보낸 값\" 표도 멈춰요.");

    ImGui::SeparatorText("삼각형 모양이 오는 길");
    ImGui::Checkbox("비교용: 정점 버퍼 없이 (SV_VertexID)", &m_noBuffer);
    Help("끔(기본) = 게임 방식. 삼각형 꼭짓점 3개(12바이트씩)를 정점 버퍼로 VRAM(Default 힙)에 한 번 올려 두고, 삼각형 3개가 같은 버퍼를 같이 써요. [회전-⑦]\n"
         "켬 = 비교용. 꼭짓점을 spin.hlsl 의 SpinVSNoBuffer 안에 직접 적어 두고 번호(SV_VertexID)로 골라요. 장면 2처럼요.\n"
         "그림은 똑같아요. 다른 건 모양을 어디서 가져오느냐뿐이에요. 자세한 비교는 장면 5 (정점 버퍼).");
}

void SpinScene::PanelShader()
{
    ImGui::SeparatorText("셰이더 (spin.hlsl)");
    if (ImGui::Button(m_sh.editorOpen ? "셰이더 창 닫기 (F2)" : "셰이더 창 열기 (F2)")) m_sh.editorOpen = !m_sh.editorOpen;
    ImGui::SameLine();
    if (ImGui::Button("다시 컴파일 (F5)")) m_sh.RequestRebuild();
    for (int m = 0; m < kMethodCount; m++)
    {
        if (m) ImGui::SameLine();
        if (Pso(m)) ImGui::TextColored(ImVec4(0.55f, 1, 0.55f, 1), "%c 됨", 'A' + m);
        else          ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%c PSO 실패", 'A' + m);
    }
    ImGui::SameLine();
    Help("셰이더 파일은 하나인데 PSO(셰이더 + 설정 묶음)는 방법마다 하나씩, 3개를 만들어요. 루트 시그니처가 PSO 안에 들어가거든요.\n"
         "\"됨\"은 PSO 를 만드는 데 성공했다는 뜻이에요. b1, t0 처럼 가방에 없는 자리를 쓰면 실패해요.\n"
         "하지만 크기가 모자라는 건 막아 주지 않아요. 아래 \"b0 크기 검사\"를 보세요.");
    PanelB0Check();
    ImGui::TextDisabled("파일 맨 위 주석에 실험 ①~③ 이 있어요");
}

// 셰이더가 b0 에서 읽는 크기 vs 방법마다 b0 에 준비된 크기
void SpinScene::PanelB0Check()
{
    const UINT bagBytes = sizeof(SpinConstants);   // A: 루트 상수 12개 = 48바이트
    if (m_b0Used == 0)
    {
        ImGui::TextDisabled("b0 크기 검사: 셰이더 정보를 못 읽었어요");
        return;
    }
    ImGui::Text("b0 크기 검사: 셰이더가 읽는 양 %u바이트", m_b0Used);
    AboutHelp("셰이더 리플렉션(D3DReflect)으로 컴파일된 셰이더에게 물어본 값이에요.\n"
              "\"읽는 바이트\"는 셰이더가 실제로 쓰는 값까지만 세요. cbuffer 에 적혀만 있고 아무도 안 읽는 값은 컴파일러가 없는 셈 쳐요.\n"
              "A 는 가방에 숫자 12개(48바이트)만 넣어요. 셰이더가 그보다 많이 읽으면 넘친 자리는 아무도 정하지 않은 값이에요 (0 일 수도, 쓰레기일 수도).\n"
              "D3D12 는 PSO 를 만들 때 이걸 막아 주지 않아서, 이렇게 직접 검사하는 거예요.\n"
              "B, C 는 256바이트 칸을 가리키니까 64바이트까지 읽어도 칸 안이에요 (C++ 이 안 쓴 자리는 0).");
    ImGui::TextDisabled("(cbuffer 에 적힌 크기는 %u바이트. 안 읽는 값은 안 세요)", m_b0Declared);
    if (m_b0Used > bagBytes)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.45f, 1));
        ImGui::TextWrapped("A: 가방은 %u바이트뿐인데 %u바이트를 읽어요. 넘친 %u바이트는 정해지지 않은 값이에요! (PSO 는 그래도 만들어져요)",
                           bagBytes, m_b0Used, m_b0Used - bagBytes);
        ImGui::PopStyleColor();
    }
    else
        ImGui::TextColored(ImVec4(0.55f, 1, 0.55f, 1), "A: 가방 %u바이트 안에 들어가요", bagBytes);
    ImGui::TextColored(m_b0Used <= kSlotSize ? ImVec4(0.55f, 1, 0.55f, 1) : ImVec4(1, 0.45f, 0.45f, 1),
                       "B, C: 칸 %u바이트 %s", kSlotSize, m_b0Used <= kSlotSize ? "안에 들어가요" : "를 넘어요!");
}

// 가방(루트 시그니처) 사용량 비교 표
void SpinScene::PanelBag()
{
    ImGui::SeparatorText("세 가지 길 비교");
    AboutHelp("가방 = 루트 시그니처. 64칸(칸 하나 4바이트)까지만 담을 수 있어요.\n"
              "숫자가 사는 곳 = 그리기 전에 그 숫자가 들어 있는 메모리.\n"
              "GPU 가 찾아가는 길 = 셰이더가 b0 을 읽을 때 GPU 가 몇 번 건너가야 숫자에 닿는지.");
    if (!ImGui::BeginTable("bag", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("방법");
    ImGui::TableSetupColumn("가방 칸");
    ImGui::TableSetupColumn("숫자가 사는 곳");
    ImGui::TableSetupColumn("GPU 가 찾아가는 길");
    ImGui::TableHeadersRow();
    const char* rows[kMethodCount][4] = {
        { "A 루트 상수", "12 / 64", "명령 목록 안", "가방에서 바로" },
        { "B 루트 CBV", "2 / 64", "Upload 버퍼 칸", "가방 -> 주소 -> 숫자" },
        { "C 테이블", "1 / 64", "Upload 버퍼 칸", "가방 -> 주소록 칸 -> 주소 -> 숫자" },
    };
    for (int m = 0; m < kMethodCount; m++)
    {
        ImGui::TableNextRow();
        if (m == m_method) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(70, 90, 140, 160));
        for (int j = 0; j < 4; j++) { ImGui::TableNextColumn(); ImGui::TextUnformatted(rows[m][j]); }
    }
    ImGui::EndTable();
}

// 지금 보낸 값 (삼각형마다)
void SpinScene::PanelValues()
{
    ImGui::SeparatorText("지금 보낸 값");
    AboutHelp("이번 프레임에 삼각형마다 보낸 숫자예요. 각도는 cos, sin 을 다시 각도로 바꿔서 보여 줘요.\n"
              "\"어디로\"는 숫자가 들어간 곳이에요. A 는 주문서 안, B 와 C 는 Upload 버퍼의 몇 번째 칸인지.\n"
              "B, C 는 칸 번호가 두 개예요 (예: 칸 0 / 3). 프레임마다 세트 0 과 세트 1 을 번갈아 쓰거든요. 앞이 세트 0, 뒤가 세트 1 이에요. 괄호 안 +숫자는 버퍼 맨 앞에서 몇 바이트 떨어진 곳인지예요.\n"
              "(지금 쓰는 칸 하나만 보여 주면 글자가 매 프레임 바뀌어서 두 값이 겹쳐 보여요.)");
    if (!ImGui::BeginTable("vals", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    // 열 너비를 비율로 고정해요. 글자 길이에 맞춰 너비가 매 프레임 바뀌면 표가 흔들려요.
    ImGui::TableSetupColumn("삼각형", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("각도", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("(cos, sin)", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("어디로 (세트 0 / 1)", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableHeadersRow();
    for (UINT i = 0; i < kDraws; i++)
    {
        const SpinConstants& k = m_last[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::ColorButton("##c", ImVec4(k.color[0], k.color[1], k.color[2], 1), ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine(); ImGui::Text("%u", i + 1);
        ImGui::TableNextColumn();
        float deg = std::atan2(k.cosSin[1], k.cosSin[0]) * 57.29578f;
        if (deg < 0) deg += 360.0f;
        ImGui::Text("%.0f도", deg);
        ImGui::TableNextColumn(); ImGui::Text("(%.2f, %.2f)", k.cosSin[0], k.cosSin[1]);
        ImGui::TableNextColumn();
        // 세트 0 의 칸과 세트 1 의 칸을 둘 다 보여 줘요 (SlotFor 와 같은 계산)
        const UINT s0 = m_oneSlot ? 0 : i;
        const UINT s1 = kDraws + s0;
        if (m_method == kRootConst) ImGui::TextUnformatted("주문서 안");
        else if (m_method == kRootCbv) ImGui::Text("칸 %u / %u (+%u / +%u)", s0, s1, s0 * kSlotSize, s1 * kSlotSize);
        else ImGui::Text("주소록 #%u / #%u -> 칸 %u / %u", m_cbvIndex[s0], m_cbvIndex[s1], s0, s1);
    }
    ImGui::EndTable();
}

// Upload 버퍼 칸 배치 (256바이트 정렬)
void SpinScene::PanelSlots()
{
    ImGui::SeparatorText("Upload 버퍼 칸 (B, C 가 씀)");
    AboutHelp("Upload 버퍼 하나를 256바이트 칸 6개로 나눠 써요: 프레임 자원 세트 2개 x 삼각형 3개.\n"
              "보낼 숫자는 48바이트뿐인데 칸은 256바이트예요. D3D12 규칙상 상수 버퍼(CBV)는 256바이트 단위로만 가리킬 수 있어서, 나머지 208바이트는 빈칸이에요.\n"
              "프레임 세트를 2개 두는 이유: CPU 가 다음 프레임 값을 쓰는 동안 GPU 는 아직 이전 프레임을 그리고 있을 수 있어요. 같은 칸이면 GPU 가 읽는 도중에 값이 바뀌어요.\n"
              "지금 이 프레임이 쓰는 칸은 밝게 보여요.");
    const UINT used = sizeof(SpinConstants);
    ImGui::Text("한 칸 %u바이트 = 값 %u바이트 + 빈칸 %u바이트", kSlotSize, used, kSlotSize - used);
    if (!ImGui::BeginTable("slots", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("칸");
    ImGui::TableSetupColumn("위치");
    ImGui::TableSetupColumn("프레임 세트");
    ImGui::TableSetupColumn("삼각형");
    ImGui::TableHeadersRow();
    const bool usesBuffer = m_method != kRootConst;
    for (UINT s = 0; s < kSlots; s++)
    {
        bool live = false;
        for (UINT i = 0; i < kDraws; i++) if (usesBuffer && m_lastSlot[i] == s) live = true;
        ImGui::TableNextRow();
        if (live) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(70, 90, 140, 160));
        ImGui::TableNextColumn(); ImGui::Text("%u", s);
        ImGui::TableNextColumn(); ImGui::Text("+%u", s * kSlotSize);
        ImGui::TableNextColumn(); ImGui::Text("%u", s / kDraws);
        ImGui::TableNextColumn();
        if (m_oneSlot && usesBuffer) ImGui::TextUnformatted(s % kDraws == 0 ? "1, 2, 3 (같이 씀!)" : "-");
        else ImGui::Text("%u", s % kDraws + 1);
    }
    ImGui::EndTable();
}

void SpinScene::PanelEli5()
{
    ImGui::SeparatorText("쉽게 설명 (눌러서 펼치기)");
    Eli5("루트 시그니처 (Root Signature)",
         "셰이더에게 주는 준비물 가방이에요. 가방에 칸이 몇 개 있고 칸마다 무엇이 들어가는지 미리 정해 둬요.",
         "\"이 셰이더의 b0 자리에는 이런 식으로 값을 넣어 줄게\"라는 약속서. 64칸(칸 하나 4바이트)까지.",
         "C++ 이 SetGraphicsRoot... 함수로 칸을 채우면, 셰이더가 b0 을 읽을 때 GPU 가 그 칸을 보고 값을 찾아가요.",
         "GPU 는 셰이더를 돌리기 전에 \"값이 어디 있지?\"를 알아야 해요. 미리 약속해 두면 그릴 때마다 묻지 않고 바로 찾아가요.",
         "이 장면은 같은 셰이더에 가방 3종류를 만들어요. 셰이더 코드는 하나도 안 바뀌고, 가방 모양과 C++ 함수만 바뀌어요.");
    Eli5("루트 상수 (방법 A)",
         "쪽지에 숫자를 직접 적어서 가방에 넣기.",
         "숫자 자체를 가방 칸에 넣는 것. SetGraphicsRoot32BitConstants(칸, 개수, 값).",
         "숫자가 명령 목록(주문서) 안에 복사돼요. 그리기마다 다른 숫자를 넣어도 각각 그대로 남아요.",
         "몇 개 안 되는 숫자를 가장 빠르고 간단하게 넘기려고. 메모리를 따로 만들 필요가 없어요.",
         "장면 1, 장면 2 가 이 방법을 써요 (iTime 등 8개). 여기서는 12개.");
    Eli5("Upload 버퍼와 루트 CBV (방법 B)",
         "사물함(Upload 버퍼)에 숫자를 넣어 두고, 가방에는 사물함 주소만 적어 넣기.",
         "CBV = Constant Buffer View, 상수 버퍼를 가리키는 것. 루트 CBV 는 그 GPU 주소를 가방에 바로 넣는 것.",
         "CPU 가 Map 해 둔 Upload 버퍼 칸에 memcpy 로 48바이트를 쓰고, SetGraphicsRootConstantBufferView(칸, 주소) 로 주소를 넘겨요.",
         "가방이 작아서 큰 값(행렬 여러 개, 조명 수십 개)은 못 넣어요. 사물함은 칸 하나에 64KB 까지 돼요.",
         "발표의 Upload 힙이 바로 이 버퍼예요. CPU 가 쓰고 GPU 가 PCIe 너머로 읽어요. 자주 바뀌는 작은 값(매 프레임 행렬)은 보통 여기 둬요.");
    Eli5("디스크립터와 디스크립터 테이블 (방법 C)",
         "사물함 주소를 카드(디스크립터)에 적어 주소록(디스크립터 힙)에 꽂아 두고, 가방에는 주소록 몇 번째 칸인지만 적기.",
         "디스크립터 = \"이 메모리를 이렇게 읽어\"라는 카드 (주소 + 크기 + 종류). 디스크립터 힙 = 카드를 꽂는 주소록. 테이블 = 주소록의 칸 번호.",
         "CreateConstantBufferView 로 카드를 미리 만들어 두고, 그릴 때는 SetDescriptorHeaps(주소록) + SetGraphicsRootDescriptorTable(칸 번호).",
         "칸 번호 하나로 카드 여러 장을 한꺼번에 넘길 수 있어요. 그리고 텍스처는 카드로만 넘길 수 있어요.",
         "발표 2부의 디스크립터 이야기가 이거예요. 로드맵 3단계(텍스처)에서 SRV 카드로 다시 만나요.");
    Eli5("256바이트 칸 (정렬)",
         "주차장 한 칸에 차 한 대. 경차(48바이트)여도 한 칸(256바이트)을 다 차지해요.",
         "상수 버퍼를 가리킬 때 시작 위치와 크기가 256 의 배수여야 한다는 D3D12 규칙.",
         "그래서 삼각형마다 0, 256, 512 ... 바이트 위치에 값을 써요.",
         "GPU 가 상수를 큰 덩어리로 한 번에 읽어 오도록 하드웨어에 맞춘 규칙이에요.",
         "아래 \"Upload 버퍼 칸\" 표의 위치 열이 모두 256 의 배수예요.");
    Eli5("한 칸만 쓰면 왜 틀리나 (CPU 와 GPU 의 시간 차이)",
         "식당에서 주문서를 다 쓰고 나서 주방에 넘기는 것. 주문서에 \"3번 사물함의 재료로\"라고만 적었는데, 주문서를 쓰는 동안 3번 사물함 재료를 세 번 바꿔 넣었다면? 주방은 마지막 재료로 세 접시를 다 만들어요.",
         "명령 목록은 GPU 에게 줄 주문서예요. CPU 가 다 쓴 뒤 한꺼번에 넘기고, GPU 는 그 뒤에 실행해요.",
         "방법 A 는 숫자를 주문서에 직접 적어서 괜찮아요. B, C 는 \"이 칸을 봐\"라고만 적으니, 칸 내용이 GPU 가 실행할 때의 값이어야 해요.",
         "CPU 와 GPU 가 따로 일하니까 \"CPU 가 쓴 시점\"과 \"GPU 가 읽는 시점\"이 달라요. 그래서 칸을 그리기마다, 프레임마다 따로 둬요.",
         "\"일부러 틀리기\"를 켜고 B 를 보세요. 3번 그렸는데 파랑 하나만 보여요. A 로 바꾸면 다시 3개가 보여요.");
}

void SpinScene::PanelCodeFlow()
{
    if (!ImGui::CollapsingHeader("이 장면의 DX12 코드 흐름")) return;
    ImGui::Indent();
    ImGui::TextWrapped("[회전-①] SpinConstants: 보낼 숫자 12개. HLSL 의 cbuffer 와 순서·크기가 같아야 해요.");
    ImGui::TextWrapped("[회전-②] CreateRootSigs(): 가방 3종류. b0 자리 하나인 건 같고, 그 칸에 숫자 / 주소 / 주소록 칸 번호 중 무엇을 넣는지만 달라요.");
    ImGui::TextWrapped("[회전-③] CreateUploadBuffer(): Upload 힙 버퍼 하나(256 x 6 바이트)를 만들고 Map 해 둬요.");
    ImGui::TextWrapped("[회전-④] CreateCbvDescriptors(): 칸 6개마다 CBV 카드를 만들어 주소록(SRV 힙)에 꽂아요. 방법 C 만 써요.");
    ImGui::TextWrapped("[회전-⑤] MakeConstants(): 시간 -> 각도 -> cos, sin. 위치, 크기, 색도 채워요.");
    ImGui::TextWrapped("[회전-⑦] Init() 의 CreateMesh + DrawTriangle(): 삼각형 모양(12바이트 x 3)을 정점 버퍼로 VRAM 에 한 번 올려 두고, 삼각형 3개가 같이 써요. 게임 방식이에요. 비교용 체크 상자를 켜면 SV_VertexID 로 셰이더 안의 점을 써요.");
    ImGui::TextWrapped("[회전-⑥] OnRender(): DrawWithRootConstants / DrawWithRootCbv / DrawWithTable 중 하나. 세 함수를 나란히 놓고 비교해 보세요. 다른 건 값 넘기는 한 줄뿐이에요.");
    ImGui::Unindent();
}

// ---------------------------------------------------------------------
// 개발용 자동 테스트 (LAB_AUTOTEST=1, LAB_SCENE=2)
//  삼각형 3개의 중심 픽셀을 되읽어서, 기대한 색(빨강, 초록, 파랑 또는 배경)인지 본다.
// ---------------------------------------------------------------------
static const int kTestCases = 6;   // (방법 A, B, C) x (일부러 틀리기 끔, 켬)

void SpinScene::ApplyTestCase(int k)
{
    m_method = k % kMethodCount;
    m_oneSlot = k >= kMethodCount;
    m_testWait = 0;
}

// 그린 뒤 백 버퍼에서 삼각형 중심 3픽셀을 Readback 버퍼로 복사 (2프레임 뒤 OnFrameReused 에서 읽음)
void SpinScene::CaptureCenters(ID3D12GraphicsCommandList* cl, const Canvas& c)
{
    const UINT f = g_dx.frameIndex;
    m_capTag[f] = m_testCase;
    ID3D12Resource* back = g_dx.backBuffers[g_dx.swapChain->GetCurrentBackBufferIndex()].Get();
    D3D12_RESOURCE_BARRIER b = Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &b);
    for (UINT i = 0; i < kDraws; i++)
    {
        const UINT px = (UINT)((kOffsetX[i] * 0.5f + 0.5f) * c.w), py = (UINT)(0.5f * c.h);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = back; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        dst.pResource = m_readback[f].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 512ull * i;
        dst.PlacedFootprint.Footprint = { kBackBufferFormat, 1, 1, 1, 256 };
        D3D12_BOX box{ px, py, 0, px + 1, py + 1, 1 };
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
}

void SpinScene::OnFrameReused(UINT slot)
{
    if (!m_readback[0] || m_capTag[slot] < 0) return;
    uint8_t* p = nullptr;
    D3D12_RANGE r{ 0, 512 * kDraws };
    if (FAILED(m_readback[slot]->Map(0, &r, (void**)&p))) return;
    for (UINT i = 0; i < kDraws; i++)
    {
        const uint8_t* px = p + 512 * i;
        for (int j = 0; j < 3; j++) m_gotRgb[i][j] = px[j];
        const unsigned mx = std::max(px[0], std::max(px[1], px[2]));
        m_gotClass[i] = mx < 60 ? -1 : (px[0] == mx ? 0 : px[1] == mx ? 1 : 2);   // -1 = 배경
    }
    D3D12_RANGE none{ 0, 0 };
    m_readback[slot]->Unmap(0, &none);
    m_gotTag = m_capTag[slot];
    m_capTag[slot] = -1;
}

bool SpinScene::AutoTest(int frame, int& failures)
{
    if (frame < 3) return false;
    if (m_testCase < 0) { m_testCase = 0; ApplyTestCase(0); m_paused = true; return false; }
    if (++m_testWait > 60)
    {
        failures++;
        FILE* f = fopen("lab_autotest.log", "a"); if (f) { fprintf(f, "spin case %d TIMEOUT\n", m_testCase); fclose(f); }
    }
    else if (m_gotTag != m_testCase || m_testWait < 4) return false;
    else
    {
        // 기대: 일부러 틀리기 + B/C 이면 마지막(파랑) 자리에만 하나. 아니면 빨강, 초록, 파랑 각자 자리에
        const bool merged = m_oneSlot && m_method != kRootConst;
        const int expect[kDraws] = { merged ? -1 : 0, merged ? -1 : 1, 2 };
        bool ok = true;
        for (UINT i = 0; i < kDraws; i++) ok &= m_gotClass[i] == expect[i];
        if (!ok) failures++;
        FILE* f = fopen("lab_autotest.log", "a");
        if (f)
        {
            fprintf(f, "spin case %d method=%s oneSlot=%d", m_testCase, kMethodName[m_method], (int)m_oneSlot);
            for (UINT i = 0; i < kDraws; i++) fprintf(f, " [%u]=(%u,%u,%u)->%d/%d", i, m_gotRgb[i][0], m_gotRgb[i][1], m_gotRgb[i][2], m_gotClass[i], expect[i]);
            fprintf(f, " %s\n", ok ? "ok" : "FAIL");
            fclose(f);
        }
    }
    if (++m_testCase >= kTestCases) return true;
    ApplyTestCase(m_testCase);
    m_gotTag = -1;
    return false;
}

} // namespace

Scene* CreateSpinScene() { return new SpinScene(); }
