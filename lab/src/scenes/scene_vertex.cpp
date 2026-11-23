// =====================================================================
//  장면 2: 정점 버퍼 - 점을 GPU 에 넘기는 실제 게임 방식
//
//  장면 1 은 점 위치를 루트 상수로 보내고 셰이더가 번호(SV_VertexID)로 꺼냈다 (배우기용 시각화).
//  게임은 모델 파일에서 읽은 점을 "정점 버퍼"에 담아 넘긴다. 이 장면은 그 길을 끝까지 보여 준다.
//  같은 도형을 세 가지 길로 그려서 비교한다 (오른쪽 패널에서 바꾸기):
//
//    방법 A  게임 방식      : 정점·인덱스 버퍼를 Default 힙(VRAM)에. Upload 짐 칸 -> GPU 복사 -> 펜스
//    방법 B  최소 방식(비교용): 정점 버퍼를 Upload 힙(RAM)에 하나만. Microsoft HelloTriangle 과 같다
//    방법 C  버퍼 없음(비교용): 점 위치를 셰이더에 직접 적기 (SV_VertexID). 장면 3 놀이터가 쓰는 방법
//
//  셰이더: lab/shaders/vb.hlsl (F2 셰이더 창)
//  올리는 코드: src/mesh.h, src/mesh.cpp (다른 장면도 같이 쓰는 도우미)
//
//  코드에서 Ctrl+F 로 찾기 (읽는 순서대로):
//    [정점-①] 정점 구조체 + 입력 레이아웃 (시맨틱)          이 파일, vb.hlsl
//    [정점-②] Default 버퍼 + Upload 짐 칸 만들기            mesh.cpp
//    [정점-③] CPU 가 Upload 짐 칸에 쓰기                    mesh.cpp
//    [정점-④] GPU 복사 (CopyBufferRegion) + 배리어          mesh.cpp
//    [정점-⑤] 실행하고 펜스로 기다리기                       mesh.cpp
//    [정점-⑥] 뷰 만들기 (주소, 전체 크기, stride)            mesh.cpp
//    [정점-⑦] 루트 시그니처 (입력 레이아웃 허용) + PSO       이 파일
//    [정점-⑧] 그리기: IASetVertexBuffers, IASetIndexBuffer, DrawIndexedInstanced   이 파일, mesh.cpp
//    [정점-⑨] 비교용 최소 방식: Upload 힙 하나로 끝          mesh.cpp
// =====================================================================
#include "../scene.h"
#include "../mesh.h"
#include "../shader_file.h"
#include "../ui_help.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <string>

namespace {

// ===== [정점-①] 정점 하나 = 28바이트. vb.hlsl 의 VSInput 과 모양이 같아야 한다 =====
struct Vertex
{
    float position[3];   // POSITION : x, y, z  (12바이트)
    float color[4];      // COLOR    : r, g, b, a (16바이트)
};
static_assert(sizeof(Vertex) == 28, "float 7개 = 28바이트");

// 입력 레이아웃 = "28바이트를 어떻게 잘라서 VSInput 의 어느 칸에 넣을지" 설명서. 줄 하나 = 칸 하나, 7개 값.
//   SemanticName, SemanticIndex : HLSL 의 POSITION, COLOR (이름 + 번호. COLOR0 이면 "COLOR", 0)
//   Format                      : 한 칸의 모양 (R32G32B32_FLOAT = float 3개)
//   InputSlot                   : 몇 번 정점 버퍼에서 (IASetVertexBuffers 의 슬롯 번호)
//   AlignedByteOffset           : 정점 한 개 안에서 몇 번째 바이트부터
//   InputSlotClass, InstanceDataStepRate : 정점마다 다음 값으로 (인스턴싱이면 PER_INSTANCE)
static const D3D12_INPUT_ELEMENT_DESC kInputLayout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },   // 0 바이트부터
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },   // 12 바이트부터
};
// 시작 바이트를 손으로 적었으니, 구조체가 바뀌면 컴파일 때 알려 주게 한다
static_assert(offsetof(Vertex, position) == 0 && offsetof(Vertex, color) == 12, "kInputLayout 의 시작 바이트를 고치세요");

// 도형 데이터 (실제 게임에서는 모델 파일에서 읽어 온다). 위치는 -1..1 (가운데 0, 위 +1)
static const Vertex kTri[3] =
{
    { {  0.0f,  0.5f, 0.0f }, { 1, 0, 0, 1 } },   // 0 위 (빨강)
    { {  0.5f, -0.5f, 0.0f }, { 0, 1, 0, 1 } },   // 1 오른쪽 아래 (초록)
    { { -0.5f, -0.5f, 0.0f }, { 0, 0, 1, 1 } },   // 2 왼쪽 아래 (파랑)
};
static const uint16_t kTriIdx[3] = { 0, 1, 2 };

// 사각형: 점은 4개뿐이고, 삼각형 2개를 "번호표"(인덱스)로 만든다 -> 0 1 2 | 0 2 3
static const Vertex kQuad[4] =
{
    { { -0.5f,  0.5f, 0.0f }, { 1, 0, 0, 1 } },   // 0 왼쪽 위 (빨강)
    { {  0.5f,  0.5f, 0.0f }, { 0, 1, 0, 1 } },   // 1 오른쪽 위 (초록)
    { {  0.5f, -0.5f, 0.0f }, { 0, 0, 1, 1 } },   // 2 오른쪽 아래 (파랑)
    { { -0.5f, -0.5f, 0.0f }, { 1, 1, 0, 1 } },   // 3 왼쪽 아래 (노랑)
};
static const uint16_t kQuadIdx[6] = { 0, 1, 2, 0, 2, 3 };   // 시계 방향 (D3D 기본 앞면)

// 인덱스 버퍼가 없으면 같은 점을 두 번 적어야 한다 (0 과 3, 2 와 4 가 같은 점)
static const Vertex kQuadNoIdx[6] = { kQuad[0], kQuad[1], kQuad[2], kQuad[0], kQuad[2], kQuad[3] };

enum Method { kGame = 0, kMinimal = 1, kNoBuffer = 2, kMethodCount = 3 };
static const char* kMethodName[kMethodCount] = { "A 게임 방식", "B 최소 방식 (비교용)", "C 버퍼 없음 (비교용)" };
static const char* kMethodShort[kMethodCount] = { "Default 힙(VRAM) + 인덱스 버퍼", "Upload 힙(RAM) 하나, HelloTriangle 과 같음", "셰이더에 점을 직접 적기, SV_VertexID" };
enum Shape { kTriangle = 0, kQuadShape = 1, kShapeCount = 2 };
static const char* kShapeName[kShapeCount] = { "삼각형", "사각형" };

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

class VertexScene : public Scene
{
public:
    const char* Name() const override { return "장면 2: 정점 버퍼 (점을 GPU 에 넘기는 게임 방식)"; }
    void Init() override;
    void OnPanel() override;
    void OnCanvas(const Canvas& c) override;
    void OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c) override;
    void OnMinimized(const Canvas& c) override;
    bool AutoTest(int frame, int& failures) override;

private:
    ShaderFile                  m_sh;                     // lab/shaders/vb.hlsl
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_psoIA;                  // A, B: 입력 레이아웃 있음 (VSMain)
    ComPtr<ID3D12PipelineState> m_psoNoBuf[kShapeCount];  // C: 입력 레이아웃 없음 (VSNoBufferTri / Quad)
    Mesh                        m_mesh[2][kShapeCount];   // [A 또는 B][모양]

    int m_method = kGame;
    int m_shape  = kQuadShape;

    void CreateRootSig();
    void CreateMeshes();
    bool BuildPipelines();
    ComPtr<ID3D12PipelineState> MakePso(ID3DBlob* vs, ID3DBlob* ps, bool useInputLayout, std::string& err);
    const Mesh* CurrentMesh() const { return m_method == kNoBuffer ? nullptr : &m_mesh[m_method][m_shape]; }
    void Square(const Canvas& c, float& x, float& y, float& side) const;
    void DrawCanvasOverlay(const Canvas& c);

    void PanelMethod();
    void PanelNow();
    void PanelLayout();
    void PanelData();
    void PanelShader();
    void PanelEli5();
    void PanelCodeFlow();

    int m_testCase = -1, m_testWait = 0;
};

// ---------------------------------------------------------------------
// GPU 자원 (Samples 의 LoadAssets)
// ---------------------------------------------------------------------
void VertexScene::Init()
{
    CreateRootSig();    // [정점-⑦]
    CreateMeshes();     // [정점-②~⑥, ⑨] (mesh.cpp)
    m_sh.Open("vb.hlsl");
    m_sh.editorOpen = false;   // 이 장면의 주인공은 패널. 셰이더 창은 F2 로
    BuildPipelines();
}

void VertexScene::CreateMeshes()
{
    // A 게임 방식: Default 힙 + 인덱스 버퍼 (삼각형도 번호표 0 1 2 를 둔다. 게임은 거의 늘 인덱스로 그린다)
    m_mesh[kGame][kTriangle]    = CreateMesh(kTri,  sizeof(Vertex), 3, kTriIdx,  3, MeshHeap::Default, L"A triangle");
    m_mesh[kGame][kQuadShape]   = CreateMesh(kQuad, sizeof(Vertex), 4, kQuadIdx, 6, MeshHeap::Default, L"A quad");
    // B 최소 방식 (비교용): Upload 힙, 인덱스 버퍼 없음 -> 사각형은 점 6개
    m_mesh[kMinimal][kTriangle]  = CreateMesh(kTri,       sizeof(Vertex), 3, nullptr, 0, MeshHeap::Upload, L"B triangle");
    m_mesh[kMinimal][kQuadShape] = CreateMesh(kQuadNoIdx, sizeof(Vertex), 6, nullptr, 0, MeshHeap::Upload, L"B quad");
}

// ===== [정점-⑦] 루트 시그니처: 넘길 상수는 없고, "입력 레이아웃을 쓸게요" 표시만 =====
void VertexScene::CreateRootSig()
{
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 0;
    // 이 표시가 없으면 IA 가 정점 버퍼를 읽지 않아요 (PSO 를 만들 때 입력 레이아웃을 줘도 안 됨)
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    m_rootSig = CreateRootSignature(rs);
}

// [정점-⑦] PSO: A, B 는 입력 레이아웃을 넣고, C 는 비워 둔다. 나머지는 같다
ComPtr<ID3D12PipelineState> VertexScene::MakePso(ID3DBlob* vs, ID3DBlob* ps, bool useInputLayout, std::string& err)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_rootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    if (useInputLayout) pd.InputLayout = { kInputLayout, (UINT)std::size(kInputLayout) };
    else                pd.InputLayout = { nullptr, 0 };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;    // 게임처럼 뒷면은 버림 (시계 방향 = 앞면)
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;              // 도형 하나라 깊이 버퍼는 아직 필요 없음 (로드맵 4단계)
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = kBackBufferFormat;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
    {
        err += useInputLayout
            ? "방법 A, B: 컴파일은 됐지만 PSO 를 못 만들었어요.\n흔한 이유: VSInput 의 시맨틱(POSITION, COLOR)이 C++ 입력 레이아웃의 SemanticName 과 글자가 다름.\n"
            : "방법 C: 컴파일은 됐지만 PSO 를 못 만들었어요.\n흔한 이유: VSNoBuffer 함수가 SV_VertexID 말고 다른 입력(정점 버퍼에서 올 값)을 받음.\n";
        return nullptr;
    }
    return pso;
}

bool VertexScene::BuildPipelines()
{
    ComPtr<ID3DBlob> vs, vsTri, vsQuad, ps;
    std::string err;
    bool ok = TryCompile(m_sh, "VSMain", "vs_5_0", vs, err);
    ok &= TryCompile(m_sh, "VSNoBufferTri", "vs_5_0", vsTri, err);
    ok &= TryCompile(m_sh, "VSNoBufferQuad", "vs_5_0", vsQuad, err);
    ok &= TryCompile(m_sh, "PSMain", "ps_5_0", ps, err);
    if (!ok) { m_sh.error = err; return false; }   // 문법 에러: 지금 쓰던 PSO 를 그대로 둔다

    if (m_psoIA || m_psoNoBuf[0] || m_psoNoBuf[1]) WaitIdle();   // GPU 가 옛 PSO 를 다 쓴 뒤에 바꾼다
    std::string psoErr;
    m_psoIA       = MakePso(vs.Get(), ps.Get(), true, psoErr);
    m_psoNoBuf[0] = MakePso(vsTri.Get(), ps.Get(), false, psoErr);
    m_psoNoBuf[1] = MakePso(vsQuad.Get(), ps.Get(), false, psoErr);
    m_sh.error = psoErr;
    m_sh.buildCount++;
    return psoErr.empty();
}

// ---------------------------------------------------------------------
// 왼쪽 그림 영역: 가운데 정사각형에 그린다 (찌그러지지 않게)
// ---------------------------------------------------------------------
void VertexScene::Square(const Canvas& c, float& x, float& y, float& side) const
{
    side = std::floor(std::min(c.w, c.h) * 0.9f);
    x = std::floor((c.w - side) * 0.5f);
    y = std::floor((c.h - side) * 0.5f);
}

void VertexScene::OnCanvas(const Canvas& c)
{
    DrawCanvasOverlay(c);
    DrawShaderEditor(m_sh, c);
}

void VertexScene::OnMinimized(const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipelines();
    DrawShaderEditor(m_sh, c);
}

// 꼭짓점 옆에 "정점 버퍼 안의 번호"를 적고, 인덱스가 만든 삼각형 테두리를 그린다
void VertexScene::DrawCanvasOverlay(const Canvas& c)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    const float fs = ImGui::GetFontSize();
    char line[256];
    snprintf(line, sizeof line, "지금: 방법 %s (%s) · %s · F2 셰이더 창", kMethodName[m_method], kMethodShort[m_method], kShapeName[m_shape]);
    dl->AddText(ImVec2(c.x + fs * 0.6f, c.y + fs * 0.4f), IM_COL32(255, 255, 255, 210), line);

    // 지금 그리는 점 목록 (C 는 셰이더에 적힌 것과 같은 값)
    const Vertex* v = m_shape == kTriangle ? kTri : (m_method == kGame ? kQuad : kQuadNoIdx);
    const int n = m_shape == kTriangle ? 3 : (m_method == kGame ? 4 : 6);
    const uint16_t* idx = m_method == kGame ? (m_shape == kTriangle ? kTriIdx : kQuadIdx) : nullptr;
    const int ni = m_shape == kTriangle ? 3 : 6;

    float sx, sy, side;
    Square(c, sx, sy, side);
    auto toScreen = [&](const Vertex& p) {
        return ImVec2(c.x + sx + (p.position[0] + 1.0f) * 0.5f * side, c.y + sy + (1.0f - p.position[1]) * 0.5f * side);
    };

    // 삼각형 테두리 (A 는 번호표 순서, B/C 는 점 순서 3개씩)
    for (int t = 0; t + 2 < ni; t += 3)
    {
        const int a = idx ? idx[t] : t, b = idx ? idx[t + 1] : t + 1, d = idx ? idx[t + 2] : t + 2;
        dl->AddTriangle(toScreen(v[a]), toScreen(v[b]), toScreen(v[d]), IM_COL32(255, 255, 255, 150), 1.5f);
    }
    // 번호 (같은 자리의 점은 "0, 3" 처럼 묶어서)
    for (int i = 0; i < n; i++)
    {
        bool seen = false;
        for (int j = 0; j < i; j++)
            if (v[j].position[0] == v[i].position[0] && v[j].position[1] == v[i].position[1]) seen = true;
        if (seen) continue;
        std::string label = std::to_string(i);
        for (int j = i + 1; j < n; j++)
            if (v[j].position[0] == v[i].position[0] && v[j].position[1] == v[i].position[1]) label += ", " + std::to_string(j);
        ImVec2 p = toScreen(v[i]);
        const float dx = v[i].position[0], dy = -v[i].position[1];
        const float len = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
        p.x += dx / len * fs * 1.4f; p.y += dy / len * fs * 1.4f;
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 235), label.c_str());
    }
    const float by = c.y + c.h - fs * 1.6f;
    if (idx && m_shape == kQuadShape)
        dl->AddText(ImVec2(c.x + fs * 0.6f, by), IM_COL32(255, 255, 255, 200), "숫자 = 정점 버퍼 안의 순서 · 인덱스 버퍼(번호표): 0 1 2 | 0 2 3 -> 점 4개로 삼각형 2개");
    else if (m_shape == kQuadShape)
        dl->AddText(ImVec2(c.x + fs * 0.6f, by), IM_COL32(255, 255, 255, 200), "숫자 = 점 순서 · 인덱스 버퍼가 없어서 같은 자리 점을 두 번 적어요 (0, 3 과 2, 4) -> 점 6개");
    else
        dl->AddText(ImVec2(c.x + fs * 0.6f, by), IM_COL32(255, 255, 255, 200), "숫자 = 정점 버퍼 안의 순서 (0 빨강, 1 초록, 2 파랑). 사이 색은 래스터라이저가 섞은 것");
}

// ---------------------------------------------------------------------
// GPU 명령 기록 (Samples 의 PopulateCommandList)
// ---------------------------------------------------------------------

// ===== [정점-⑧] 그리기: 방법 A, B 는 정점 버퍼를 꽂고, C 는 번호만 넘긴다 =====
void VertexScene::OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipelines();   // .hlsl 을 고쳤으면 다시 컴파일 (핫 리로드)

    float sx, sy, side;
    Square(c, sx, sy, side);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferRtv();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ sx, sy, side, side, 0, 1 };
    D3D12_RECT sc{ (LONG)sx, (LONG)sy, (LONG)(sx + side), (LONG)(sy + side) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(m_rootSig.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);   // 점 3개씩 = 삼각형 하나

    if (m_method == kNoBuffer)
    {
        // C (비교용): 정점 버퍼 없음. VS 가 SV_VertexID(0, 1, 2 ...)로 셰이더 안의 점을 고른다
        if (!m_psoNoBuf[m_shape]) return;
        cl->SetPipelineState(m_psoNoBuf[m_shape].Get());
        cl->DrawInstanced(m_shape == kTriangle ? 3 : 6, 1, 0, 0);
        return;
    }
    // A, B: IASetVertexBuffers (+ A 는 IASetIndexBuffer) -> Draw. 안은 mesh.cpp 의 Mesh::Draw
    if (!m_psoIA) return;
    cl->SetPipelineState(m_psoIA.Get());
    m_mesh[m_method][m_shape].Draw(cl);
}

// ---------------------------------------------------------------------
// 오른쪽 패널
// ---------------------------------------------------------------------
void VertexScene::OnPanel()
{
    ImGui::TextWrapped("모델의 점을 GPU 에 넘기는 \"실제 게임 방식\"(A)과, 코드를 줄인 두 가지 비교용(B, C)을 바꿔 가며 봐요. 그림은 셋 다 같아요. 다른 건 점이 어디에 있고 어떻게 왔는지예요.");
    PanelMethod();
    PanelNow();
    PanelLayout();
    PanelData();
    PanelShader();
    PanelEli5();
    PanelCodeFlow();
}

void VertexScene::PanelMethod()
{
    ImGui::SeparatorText("방법");
    for (int m = 0; m < kMethodCount; m++)
    {
        ImGui::RadioButton(kMethodName[m], &m_method, m);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", kMethodShort[m]);
    }
    Help("A 게임 방식: 정점 버퍼와 인덱스 버퍼를 Default 힙(VRAM)에 둬요. CPU 는 VRAM 에 직접 못 쓰니까 "
         "Upload 짐 칸(RAM)에 먼저 쓰고, GPU 가 복사하고, 펜스로 복사가 끝난 걸 확인한 뒤 짐 칸을 지워요. 한 번 올리면 그 뒤로는 빠르게 읽어요.\n"
         "B 최소 방식 (비교용): Microsoft HelloTriangle 과 같아요. Upload 힙에 하나만 만들고 바로 그려요. 코드는 짧지만 "
         "GPU 가 그릴 때마다 PCIe 너머 RAM 에서 읽어요. 정점이 많은 모델이면 느려져요.\n"
         "C 버퍼 없음 (비교용): 점 위치를 셰이더 안에 적어 두고 번호(SV_VertexID)로 고르는 방법. 장면 3 놀이터가 이렇게 해요. "
         "모델을 파일에서 읽는 게임에서는 못 써요. 단, 화면을 덮는 삼각형(장면 3, 5)처럼 모양이 늘 같으면 게임도 이렇게 해요.");

    ImGui::SeparatorText("모양");
    for (int s = 0; s < kShapeCount; s++)
    {
        if (s) ImGui::SameLine();
        ImGui::RadioButton(kShapeName[s], &m_shape, s);
    }
    Help("사각형 = 삼각형 2개예요. 점은 4개면 충분한데, 삼각형마다 점 3개를 적으면 6개가 돼요 (두 점이 겹침).\n"
         "A 는 점 4개 + 인덱스 버퍼(번호표 0 1 2 | 0 2 3)로 겹치는 점을 다시 적지 않아요. B, C 는 점 6개를 그대로 적어요.\n"
         "그림 영역의 숫자를 보면서 바꿔 보세요.");
}

// 지금 GPU 에 있는 버퍼 (A, B 의 진짜 값)
void VertexScene::PanelNow()
{
    ImGui::SeparatorText("지금 GPU 에 있는 것");
    AboutHelp("지금 방법이 만든 버퍼의 실제 값이에요. 뷰(View) 세 칸(주소, 전체 크기, 한 칸 크기)이 IASetVertexBuffers 로 IA 에 넘어가요.\n"
              "A 와 B 를 바꿔 보면 그림은 같은데 \"어디에 있나\"와 \"올릴 때 한 일\"이 달라요. C 는 버퍼가 아예 없어요.");
    if (!ImGui::BeginTable("now", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    auto row = [](const char* a, const char* b) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(a);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", b);
    };
    char v[160];
    const Mesh* m = CurrentMesh();
    if (!m)
    {
        row("정점 버퍼", "없음 (점 위치는 vb.hlsl 의 VSNoBuffer 함수 안에)");
        row("인덱스 버퍼", "없음");
        snprintf(v, sizeof v, "DrawInstanced(%d, 1, 0, 0)", m_shape == kTriangle ? 3 : 6); row("그리기", v);
        row("올릴 때 한 일", "없음. 대신 모양을 바꾸려면 셰이더를 고쳐야 해요");
        ImGui::EndTable();
        return;
    }
    row("어디에", m->heap == MeshHeap::Default ? "Default 힙 = VRAM (GPU 옆 메모리)" : "Upload 힙 = RAM (CPU 쪽, GPU 는 PCIe 너머에서 읽음)");
    snprintf(v, sizeof v, "0x%llX", (unsigned long long)m->vbv.BufferLocation);                   row("뷰: BufferLocation", v);
    snprintf(v, sizeof v, "%u 바이트 (정점 %u개 x 28)", m->vbv.SizeInBytes, m->vertexCount);       row("뷰: SizeInBytes", v);
    snprintf(v, sizeof v, "%u 바이트 (정점 한 개)", m->vbv.StrideInBytes);                          row("뷰: StrideInBytes", v);
    if (m->indexCount) { snprintf(v, sizeof v, "%u개 x 2바이트 = %u 바이트 (R16_UINT)", m->indexCount, m->IndexBytes()); row("인덱스 버퍼", v); }
    else row("인덱스 버퍼", "없음");
    snprintf(v, sizeof v, "%u 바이트", m->VertexBytes() + m->IndexBytes());                       row("합계", v);
    if (m->indexCount) snprintf(v, sizeof v, "DrawIndexedInstanced(%u, 1, 0, 0, 0)", m->indexCount);
    else               snprintf(v, sizeof v, "DrawInstanced(%u, 1, 0, 0)", m->vertexCount);
    row("그리기", v);
    row("올릴 때 한 일", m->heap == MeshHeap::Default
        ? "Upload 짐 칸에 memcpy -> CopyBufferRegion -> 배리어(COPY_DEST -> VERTEX_AND_CONSTANT_BUFFER / INDEX_BUFFER) -> 펜스로 기다림 -> 짐 칸 지움"
        : "Map -> memcpy -> Unmap 만 (복사, 배리어, 펜스 없음)");
    ImGui::EndTable();
}

void VertexScene::PanelLayout()
{
    ImGui::SeparatorText("정점 한 개 = 28바이트 (입력 레이아웃)");
    AboutHelp("IA(입력 조립기)는 정점 버퍼를 StrideInBytes(28)씩 잘라서, 이 표대로 VSInput 의 칸에 넣어요.\n"
              "표의 한 줄 = C++ 입력 레이아웃(D3D12_INPUT_ELEMENT_DESC)의 한 줄 = HLSL VSInput 의 한 칸.\n"
              "시맨틱(POSITION, COLOR) 글자가 양쪽에서 같아야 연결돼요. 셰이더 실험 ② 에서 일부러 틀려 볼 수 있어요.");
    if (!ImGui::BeginTable("layout", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("시맨틱");
    ImGui::TableSetupColumn("형식");
    ImGui::TableSetupColumn("시작 바이트");
    ImGui::TableSetupColumn("크기");
    ImGui::TableHeadersRow();
    const char* rows[2][4] = { { "POSITION", "R32G32B32_FLOAT (float3)", "0", "12" }, { "COLOR", "R32G32B32A32_FLOAT (float4)", "12", "16" } };
    for (auto& r : rows)
    {
        ImGui::TableNextRow();
        for (int i = 0; i < 4; i++) { ImGui::TableNextColumn(); ImGui::TextUnformatted(r[i]); }
    }
    ImGui::EndTable();
}

void VertexScene::PanelData()
{
    ImGui::SeparatorText("버퍼 안의 값");
    const Mesh* m = CurrentMesh();
    const Vertex* v = m_shape == kTriangle ? kTri : (m_method == kGame ? kQuad : kQuadNoIdx);
    const int n = m_shape == kTriangle ? 3 : (m_method == kGame ? 4 : 6);
    if (!m) ImGui::TextDisabled("C 는 버퍼가 없어요. 아래 값이 vb.hlsl 의 VSNoBuffer 함수 안에 적혀 있어요.");
    if (ImGui::BeginTable("data", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("번호 (바이트)");
        ImGui::TableSetupColumn("POSITION");
        ImGui::TableSetupColumn("COLOR");
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; i++)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d (%d~%d)", i, i * 28, i * 28 + 27);
            ImGui::TableNextColumn(); ImGui::Text("%.1f, %.1f, %.1f", v[i].position[0], v[i].position[1], v[i].position[2]);
            ImGui::TableNextColumn(); ImGui::Text("%.0f, %.0f, %.0f, %.0f", v[i].color[0], v[i].color[1], v[i].color[2], v[i].color[3]);
        }
        ImGui::EndTable();
    }
    if (m && m->indexCount)
    {
        const uint16_t* idx = m_shape == kTriangle ? kTriIdx : kQuadIdx;
        std::string s = "인덱스 버퍼:";
        for (UINT i = 0; i < m->indexCount; i++) s += (i && i % 3 == 0 ? " |" : "") + std::string(" ") + std::to_string(idx[i]);
        ImGui::TextUnformatted(s.c_str());
    }
}

void VertexScene::PanelShader()
{
    ImGui::SeparatorText("셰이더 (vb.hlsl)");
    if (ImGui::Button(m_sh.editorOpen ? "셰이더 창 닫기 (F2)" : "셰이더 창 열기 (F2)")) m_sh.editorOpen = !m_sh.editorOpen;
    ImGui::SameLine();
    if (ImGui::Button("다시 컴파일 (F5)")) m_sh.RequestRebuild();
    if (!m_sh.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "에러: 셰이더 창을 보세요 (화면은 마지막 성공 버전)");
    else ImGui::TextColored(ImVec4(0.55f, 1, 0.55f, 1), "컴파일 성공 (%d번째)", m_sh.buildCount);
    ImGui::TextDisabled("파일 맨 위 주석에 실험 ①~③ 이 있어요");
}

void VertexScene::PanelEli5()
{
    ImGui::SeparatorText("쉽게 설명 (눌러서 펼치기)");
    Eli5("정점 버퍼 (Vertex Buffer)",
         "점들의 명단을 적은 종이 한 장이에요. 줄마다 점 하나 (위치, 색).",
         "GPU 메모리에 있는 바이트 덩어리. 정점(꼭짓점) 데이터를 줄줄이 붙여 담아요. 여기서는 정점 하나가 28바이트.",
         "IA(입력 조립기)가 이 덩어리를 28바이트씩 잘라서 정점 셰이더에 한 개씩 넘겨요.",
         "게임의 모델(캐릭터, 건물)은 점이 수천~수백만 개예요. 셰이더에 적어 둘 수 없으니, 파일에서 읽어 버퍼에 담아 넘겨요.",
         "삼각형 = 28 x 3 = 84바이트. 이 장면의 \"지금 GPU 에 있는 것\" 표에 실제 크기와 주소가 나와요.");
    Eli5("인덱스 버퍼 (Index Buffer)",
         "명단 번호만 적은 번호표예요. \"0번, 1번, 2번 점으로 삼각형 하나, 0번, 2번, 3번 점으로 또 하나\".",
         "정점 번호(여기서는 2바이트 정수)를 줄줄이 담은 버퍼. 3개씩 끊어서 삼각형 하나.",
         "IA 가 번호표를 보고 정점 버퍼에서 그 번호의 점을 꺼내요. 같은 점을 여러 삼각형이 같이 써요.",
         "모델의 점 하나는 보통 삼각형 여러 개(평균 6개쯤)에 같이 들어가요. 점을 매번 다시 적으면 28바이트씩 늘지만, 번호는 2바이트면 돼요.",
         "사각형: 인덱스 없이 점 6개 = 168바이트, 인덱스로 점 4개 + 번호 6개 = 112 + 12 = 124바이트.");
    Eli5("뷰 (Vertex Buffer View, Index Buffer View)",
         "택배 송장이에요. \"이 창고 주소에서, 여기까지가 한 상자, 상자 하나는 28바이트\".",
         "버퍼를 어떻게 읽을지 적은 작은 구조체. 정점 버퍼 뷰 = 주소 + 전체 크기 + stride, 인덱스 버퍼 뷰 = 주소 + 전체 크기 + 번호 형식.",
         "IASetVertexBuffers, IASetIndexBuffer 로 IA 에 넘겨요. IA 는 버퍼 자체가 아니라 이 송장을 보고 읽어요.",
         "버퍼는 그냥 바이트 덩어리라 \"어디서부터 몇 바이트씩\"을 GPU 가 모르거든요. 큰 버퍼 하나를 여러 모델이 나눠 쓸 때 주소만 바꾼 뷰를 여러 개 만들 수도 있어요.",
         "위 표의 BufferLocation, SizeInBytes, StrideInBytes 가 지금 뷰의 실제 값이에요.");
    Eli5("입력 레이아웃과 시맨틱 (Input Layout, Semantic)",
         "명단 한 줄의 칸 설명이에요. \"0~11번 글자는 위치, 12~27번 글자는 색\".",
         "D3D12_INPUT_ELEMENT_DESC 배열. 줄마다 시맨틱 이름(POSITION), 형식(float3), 시작 바이트(0)를 적어요. 시맨틱은 HLSL 쪽 변수에 붙인 이름표.",
         "IA 가 28바이트를 잘라 VSInput 의 어느 칸에 넣을지 이 설명서와 시맨틱 이름으로 맞춰요.",
         "C++ 구조체와 HLSL 구조체는 서로를 몰라요. 둘을 이어 주는 약속이 필요해요.",
         "C++ \"COLOR\" + 시작 12 바이트 <-> HLSL float4 color : COLOR. 셰이더 실험 ② 에서 글자를 바꾸면 연결이 끊겨요.");
    Eli5("Default 힙과 Upload 힙",
         "Default = 주방 안 냉장고 (요리사 바로 옆, 빠름). Upload = 현관 앞 택배 상자 (집주인이 넣기 쉬움, 요리사는 멀리 가야 함).",
         "Default 힙 = GPU 전용 메모리(VRAM). Upload 힙 = CPU 가 쓸 수 있고 GPU 도 읽을 수 있는 RAM.",
         "A 는 Upload 에 잠깐 놓고 GPU 가 Default 로 옮겨요. B 는 Upload 에 그대로 두고 매번 거기서 읽어요.",
         "모델처럼 안 바뀌고 매 프레임 읽는 데이터는 가까운 VRAM 이 빨라요. 매 프레임 바뀌는 작은 값(장면 4 의 상수)은 Upload 에 두는 게 오히려 편해요.",
         "\"지금 GPU 에 있는 것\" 표의 \"어디에\" 줄. A 와 B 를 바꿔 보세요. (발표 \"DX12 메모리 구조\"의 RAM -> Upload -> VRAM 흐름과 같아요)");
    Eli5("펜스로 기다리기 (Fence)",
         "이삿짐 트럭이 짐을 다 내릴 때까지 트럭을 돌려보내면 안 되는 것.",
         "펜스 = GPU 가 \"여기까지 끝났어요\" 하고 올리는 번호표. CPU 는 그 번호가 올라올 때까지 기다릴 수 있어요.",
         "mesh.cpp 의 CreateDefaultBuffer 는 복사 명령을 보낸 뒤 WaitIdle 로 번호표를 기다리고, 그다음에 Upload 짐 칸을 지워요.",
         "명령은 보내는 순간 끝나는 게 아니라 GPU 가 나중에 해요. 그전에 짐 칸을 지우면 GPU 가 없는 메모리를 읽어요.",
         "게임은 로딩 화면에서 여러 모델을 한 명령 목록에 모아 보내고 한 번만 기다려요. 더 나아가면 복사 전용 큐로 그리기와 동시에 올려요.");
}

void VertexScene::PanelCodeFlow()
{
    if (!ImGui::CollapsingHeader("이 장면의 DX12 코드 흐름 (Ctrl+F)")) return;
    ImGui::Indent();
    ImGui::TextWrapped("[정점-①] scene_vertex.cpp: struct Vertex(28바이트) + kInputLayout. vb.hlsl 의 VSInput 과 짝.");
    ImGui::TextWrapped("[정점-②] mesh.cpp CreateDefaultBuffer: Default 버퍼(VRAM, COMMON) + Upload 짐 칸(GENERIC_READ).");
    ImGui::TextWrapped("[정점-③] mesh.cpp: Map -> memcpy -> Unmap 으로 CPU 가 짐 칸에 써요.");
    ImGui::TextWrapped("[정점-④] mesh.cpp: 배리어 COMMON -> COPY_DEST, CopyBufferRegion, 배리어 COPY_DEST -> VERTEX_AND_CONSTANT_BUFFER (인덱스는 INDEX_BUFFER).");
    ImGui::TextWrapped("[정점-⑤] mesh.cpp: ExecuteCommandLists -> WaitIdle(펜스) -> 짐 칸 지움.");
    ImGui::TextWrapped("[정점-⑥] mesh.cpp CreateMesh: 정점 버퍼 뷰(주소, 크기, stride), 인덱스 버퍼 뷰(주소, 크기, R16_UINT).");
    ImGui::TextWrapped("[정점-⑦] scene_vertex.cpp: 루트 시그니처 ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT + PSO 에 InputLayout.");
    ImGui::TextWrapped("[정점-⑧] scene_vertex.cpp OnRender + mesh.cpp Mesh::Draw: IASetVertexBuffers, IASetIndexBuffer, DrawIndexedInstanced.");
    ImGui::TextWrapped("[정점-⑨] mesh.cpp CreateUploadBufferWithData: 비교용 최소 방식 (HelloTriangle). Upload 힙 하나로 끝.");
    ImGui::Unindent();
}

// ---------------------------------------------------------------------
// 개발용 자동 테스트: 방법 3 x 모양 2 를 돌면서 PSO 와 버퍼 위치(힙 종류)를 확인
// ---------------------------------------------------------------------
bool VertexScene::AutoTest(int frame, int& failures)
{
    if (frame < 3) return false;
    if (m_testCase < 0) { m_testCase = 0; m_testWait = 0; }
    const int method = m_testCase / kShapeCount, shape = m_testCase % kShapeCount;
    m_method = method; m_shape = shape;
    if (++m_testWait < 3) return false;   // 몇 프레임 그려 본 뒤 확인

    bool ok = true;
    const char* why = "";
    if (method == kNoBuffer) { ok = m_psoNoBuf[shape] != nullptr; why = "PSO(C)"; }
    else
    {
        const Mesh& m = m_mesh[method][shape];
        D3D12_HEAP_PROPERTIES hp{}; D3D12_HEAP_FLAGS hf{};
        ok = m_psoIA && m.Valid() && SUCCEEDED(m.vb->GetHeapProperties(&hp, &hf));
        const D3D12_HEAP_TYPE want = method == kGame ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD;
        ok = ok && hp.Type == want && m.vbv.StrideInBytes == sizeof(Vertex);
        ok = ok && (method == kGame ? m.indexCount == (shape == kTriangle ? 3u : 6u) : m.indexCount == 0);
        why = "PSO/heap/stride/index";
    }
    if (!ok) failures++;
    FILE* f = fopen("lab_autotest.log", "a");
    if (f) { fprintf(f, "vertex case %d method=%d shape=%d %s %s\n", m_testCase, method, shape, why, ok ? "ok" : "FAIL"); fclose(f); }
    m_testWait = 0;
    return ++m_testCase >= kMethodCount * kShapeCount;
}

} // namespace

Scene* CreateVertexScene() { return new VertexScene(); }
