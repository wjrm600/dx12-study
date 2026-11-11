// =====================================================================
//  장면 2: 픽셀 셰이더 놀이터 - Shadertoy 를 DX12 로 옮기기 (학습 로드맵 1단계)
//
//  Shadertoy 의 정체 = "화면을 덮는 삼각형 하나 + 픽셀 셰이더 하나".
//  이 장면은 그걸 DX12 로 그대로 만든다. 왼쪽 그림 영역의 픽셀마다 mainImage 가 한 번씩 돈다.
//  iTime, iResolution, iMouse 는 루트 상수(8개)로 매 프레임 넘긴다.
//
//  고칠 곳은 lab/shaders/playground.hlsl 의 mainImage 하나 (셰이더 창 F2, 저장하면 바로 반영).
//
//  코드에서 Ctrl+F 로 찾기:
//    [놀이터-①] 루트 시그니처: 루트 상수 8개(b0) 하나뿐
//    [놀이터-②] PSO: 정점 버퍼 없음, 컬링 없음
//    [놀이터-③] 매 프레임 값 채우기: 시간, 그림 크기, 마우스
//    [놀이터-④] 그리기: 뷰포트 = 그림 영역, 정점 3개
// =====================================================================
#include "../scene.h"
#include "../shader_file.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// HLSL 의 cbuffer PlayConstants 와 순서·크기가 같아야 한다 (float 8개 = 32바이트)
struct PlayConstants { float resolution[2], time, frame, mouse[4]; };
static_assert(sizeof(PlayConstants) == 32, "root constants = 8 x 4 bytes");

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

class PlaygroundScene : public Scene
{
public:
    const char* Name() const override { return "장면 2: 픽셀 셰이더 놀이터 (Shadertoy 옮기기)"; }
    void Init() override;
    void OnPanel() override;
    void OnCanvas(const Canvas& c) override;
    void OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c) override;
    void OnMinimized(const Canvas& c) override;

private:
    ShaderFile                  m_sh;          // lab/shaders/playground.hlsl
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // 매 프레임 셰이더에 넘기는 값
    double m_time = 0.0;
    float  m_frame = 0;
    bool   m_paused = false;
    float  m_speed = 1.0f;
    float  m_mouse[4] = { 0, 0, -1, -1 };
    float  m_w = 1, m_h = 1;

    void CreateRootSig();
    bool BuildPipeline();
    void AdvanceTime();
    void HandleMouse(const Canvas& c);
    PlayConstants MakeConstants() const;

    void PanelTime();
    void PanelShader();
    void PanelValues();
    void PanelEli5();
    void PanelCodeFlow();
};

// ---------------------------------------------------------------------
// GPU 자원 (Samples 의 LoadAssets)
// ---------------------------------------------------------------------
void PlaygroundScene::Init()
{
    CreateRootSig();
    m_sh.Open("playground.hlsl");
    BuildPipeline();
}

// ===== [놀이터-①] 루트 시그니처: 셰이더에게 주는 준비물은 루트 상수 8개(b0)뿐 =====
void PlaygroundScene::CreateRootSig()
{
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.Num32BitValues = sizeof(PlayConstants) / 4;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1; rs.pParameters = &param;
    m_rootSig = CreateRootSignature(rs);
}

// ===== [놀이터-②] PSO: 정점 버퍼 없음, 컬링 없음, 깊이 없음. 셰이더만 바뀐다 =====
bool PlaygroundScene::BuildPipeline()
{
    ComPtr<ID3DBlob> vs, ps;
    std::string err;
    bool ok = TryCompile(m_sh, "FullVS", "vs_5_0", vs, err);
    ok &= TryCompile(m_sh, "MainPS", "ps_5_0", ps, err);
    if (!ok) { m_sh.error = err; return false; }   // 틀리면 에러만 보여 주고 지금 쓰던 PSO 를 그대로 둔다

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_rootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { nullptr, 0 };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = kBackBufferFormat;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
    {
        m_sh.error = "컴파일은 됐지만 PSO 를 못 만들었어요.\n흔한 이유: cbuffer 에 float 를 더 넣어서 루트 상수 8개(32바이트)를 넘김, 또는 b1, t0 처럼 루트 시그니처에 없는 레지스터를 씀.\n";
        return false;
    }
    if (m_pso) WaitIdle();   // GPU 가 옛 PSO 를 다 쓴 뒤에 바꾼다
    m_pso = pso;
    m_sh.error.clear();
    m_sh.buildCount++;
    return true;
}

// ---------------------------------------------------------------------
// 왼쪽 그림 영역: 마우스만 받고, 그림은 OnRender 에서 GPU 가 그린다
// ---------------------------------------------------------------------
void PlaygroundScene::OnCanvas(const Canvas& c)
{
    AdvanceTime();
    HandleMouse(c);
    DrawShaderEditor(m_sh, c);
}

void PlaygroundScene::AdvanceTime()
{
    if (!m_paused) m_time += ImGui::GetIO().DeltaTime * m_speed;
    m_frame += 1.0f;
}

// Shadertoy 의 iMouse 처럼: 누르고 있는 동안 xy = 마우스 위치, zw = 누른 곳. 떼면 zw 가 음수
void PlaygroundScene::HandleMouse(const Canvas& c)
{
    m_w = c.w; m_h = c.h;
    ImGui::SetNextWindowPos(ImVec2(c.x, c.y));
    ImGui::SetNextWindowSize(ImVec2(c.w, c.h));
    ImGui::Begin("##canvas2", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorScreenPos(ImVec2(c.x, c.y));
    ImGui::InvisibleButton("area", ImVec2(c.w, c.h));
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float mx = m.x - c.x, my = c.h - (m.y - c.y);   // 왼쪽 아래가 (0, 0)
    if (ImGui::IsItemActivated()) { m_mouse[2] = mx; m_mouse[3] = my; }
    if (ImGui::IsItemActive()) { m_mouse[0] = mx; m_mouse[1] = my; m_mouse[2] = std::fabs(m_mouse[2]); m_mouse[3] = std::fabs(m_mouse[3]); }
    else { m_mouse[2] = -std::fabs(m_mouse[2]); m_mouse[3] = -std::fabs(m_mouse[3]); }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    dl->AddText(ImVec2(c.x + fs * 0.6f, c.y + fs * 0.4f), IM_COL32(255, 255, 255, 200),
                "이 영역의 픽셀마다 GPU 가 mainImage 를 한 번씩 실행해요 · 누른 채 끌면 iMouse 가 바뀌어요 · F2 셰이더 창");
    ImGui::End();
}

void PlaygroundScene::OnMinimized(const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipeline();
    DrawShaderEditor(m_sh, c);
}

// ---------------------------------------------------------------------
// GPU 명령 기록 (Samples 의 PopulateCommandList)
// ---------------------------------------------------------------------

// ===== [놀이터-③] 매 프레임 값 채우기: 이게 Shadertoy 의 iResolution, iTime, iFrame, iMouse =====
PlayConstants PlaygroundScene::MakeConstants() const
{
    PlayConstants k{};
    k.resolution[0] = m_w; k.resolution[1] = m_h;
    k.time = (float)m_time;
    k.frame = m_frame;
    for (int i = 0; i < 4; i++) k.mouse[i] = m_mouse[i];
    return k;
}

// ===== [놀이터-④] 그리기: 뷰포트 = 그림 영역, 정점 3개 (화면 덮는 삼각형) =====
void PlaygroundScene::OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipeline();   // .hlsl 을 고쳤으면 다시 컴파일 (핫 리로드)
    if (!m_pso) return;                          // 처음부터 컴파일 실패: 에러는 셰이더 창에

    const PlayConstants k = MakeConstants();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferRtv();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, c.w, c.h, 0, 1 };   // 백 버퍼 기준 (0, 0) = 메인 창 왼쪽 위 = 그림 영역 왼쪽 위
    D3D12_RECT sc{ 0, 0, (LONG)c.w, (LONG)c.h };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(m_rootSig.Get());
    cl->SetGraphicsRoot32BitConstants(0, sizeof(PlayConstants) / 4, &k, 0);
    cl->SetPipelineState(m_pso.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------
// 오른쪽 패널
// ---------------------------------------------------------------------
void PlaygroundScene::OnPanel()
{
    ImGui::TextWrapped("Shadertoy 에서 보던 그 구조를 DX12 로 만든 장면이에요. playground.hlsl 의 mainImage 를 고치고 Ctrl+S 를 누르면 바로 바뀌어요.");
    PanelTime();
    PanelShader();
    PanelValues();
    PanelEli5();
    PanelCodeFlow();
}

void PlaygroundScene::PanelTime()
{
    ImGui::SeparatorText("시간");
    if (ImGui::Button(m_paused ? "재생" : "멈춤")) m_paused = !m_paused;
    ImGui::SameLine();
    if (ImGui::Button("0초로")) { m_time = 0.0; m_frame = 0; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::SliderFloat("빠르기", &m_speed, 0.0f, 4.0f, "%.2f배");
}

void PlaygroundScene::PanelShader()
{
    ImGui::SeparatorText("셰이더 (playground.hlsl)");
    if (ImGui::Button(m_sh.editorOpen ? "셰이더 창 닫기 (F2)" : "셰이더 창 열기 (F2)")) m_sh.editorOpen = !m_sh.editorOpen;
    ImGui::SameLine();
    if (ImGui::Button("다시 컴파일 (F5)")) m_sh.RequestRebuild();
    if (!m_sh.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "컴파일 에러: 셰이더 창을 보세요 (화면은 마지막 성공 버전)");
    else ImGui::TextColored(ImVec4(0.55f, 1, 0.55f, 1), "컴파일 성공 (%d번째) · 고치고 저장하면 바로 반영", m_sh.buildCount);
    ImGui::TextDisabled("파일 맨 위 주석에 실험 ①~④ 와 GLSL -> HLSL 바꾸는 표가 있어요");
}

// 지금 셰이더에 들어가는 값 (uniform = 모든 픽셀이 같은 값을 받음)
void PlaygroundScene::PanelValues()
{
    ImGui::SeparatorText("지금 셰이더가 받는 값 (루트 상수 8개)");
    if (!ImGui::BeginTable("vals", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Shadertoy");
    ImGui::TableSetupColumn("여기 (HLSL)");
    ImGui::TableSetupColumn("값");
    ImGui::TableHeadersRow();
    auto row = [](const char* a, const char* b, const char* v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(a);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(b);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
    };
    char v[96];
    snprintf(v, sizeof v, "(%.0f, %.0f)", m_w, m_h);                                  row("iResolution.xy", "gResolution", v);
    snprintf(v, sizeof v, "%.2f 초", m_time);                                          row("iTime", "gTime", v);
    snprintf(v, sizeof v, "%.0f", m_frame);                                            row("iFrame", "gFrame", v);
    snprintf(v, sizeof v, "(%.0f, %.0f)", m_mouse[0], m_mouse[1]);                     row("iMouse.xy", "gMouse.xy", v);
    snprintf(v, sizeof v, "(%.0f, %.0f)%s", m_mouse[2], m_mouse[3], m_mouse[2] < 0 ? " 뗌" : " 누름"); row("iMouse.zw", "gMouse.zw", v);
    ImGui::EndTable();
    ImGui::TextDisabled("한 프레임 동안 이 값은 모든 픽셀에 똑같아요. 픽셀마다 다른 건 fragCoord 하나뿐.");
}

void PlaygroundScene::PanelEli5()
{
    ImGui::SeparatorText("쉽게 설명 (눌러서 펼치기)");
    Eli5("화면을 덮는 삼각형 (Fullscreen Triangle)",
         "도화지 한 장을 그림 영역 전체에 까는 거예요. 도화지가 깔린 곳의 모든 칸에 PS 가 한 번씩 붓질해요.",
         "화면보다 큰 삼각형 하나. 꼭짓점이 (-1, 1), (3, 1), (-1, -3) 이라 화면 밖은 래스터라이저가 잘라 내고, 안쪽은 빈틈없이 덮여요.",
         "그림 영역의 모든 픽셀에 대해 MainPS 가 딱 한 번씩 실행되게 해요. 정점 버퍼도 필요 없어요 (SV_VertexID 로 꼭짓점을 만듦).",
         "픽셀 셰이더만으로 그림을 그리려면 \"모든 픽셀에서 PS 를 돌려 줘\"가 필요한데, GPU 는 삼각형을 그려야만 PS 를 돌려요. 그래서 화면을 덮는 삼각형을 그려요.",
         "Shadertoy 도 속에서 똑같이 해요. 장면 1의 BlitVS 와 같은 코드예요. 사각형(삼각형 2개)보다 대각선 이음매가 없어서 조금 더 빨라요.");
    Eli5("fragCoord 와 uv",
         "모눈종이에서 \"왼쪽 아래에서 오른쪽으로 몇 칸, 위로 몇 칸\"이라는 내 자리 번호예요.",
         "fragCoord = 이 픽셀의 위치(픽셀 단위, 왼쪽 아래가 0). uv = fragCoord / iResolution.xy 로 0..1 로 줄인 것.",
         "픽셀마다 유일하게 다른 입력이에요. 이 값으로 \"나는 원 안인가\", \"무슨 색인가\"를 계산해요.",
         "모든 픽셀이 같은 함수를 돌리니까, 서로 다른 그림이 나오려면 자기 위치를 알아야 해요.",
         "그림 영역이 1200 x 800 이면 가운데 픽셀의 fragCoord 는 (600.5, 400.5), uv 는 약 (0.5, 0.5). DX12 의 SV_Position 은 위가 0 이라 MainPS 에서 y 를 뒤집어 넘겨요.");
    Eli5("uniform (루트 상수)",
         "칠판에 오늘 날짜를 적어 두면 반 학생 모두가 같은 날짜를 봐요.",
         "한 번 그리는 동안 모든 픽셀이 똑같이 받는 값. Shadertoy 의 iTime, iResolution, iMouse. 여기서는 cbuffer PlayConstants (루트 상수 8개).",
         "C++ 이 매 프레임 SetGraphicsRoot32BitConstants 로 8개 숫자를 GPU 에 넘겨요. 셰이더는 그냥 변수처럼 읽어요.",
         "시간이나 마우스처럼 \"그 순간의 상황\"을 셰이더에 알려 줄 길이 필요해요. 픽셀마다 따로 보낼 필요가 없으니 한 번만 보내요.",
         "위 표의 값이 지금 셰이더에 들어가는 실제 숫자예요. 멈춤을 누르면 gTime 이 멈추고 원도 멈춰요. 로드맵 2단계에서 루트 상수, 루트 CBV, 디스크립터 테이블을 비교해요.");
    Eli5("SDF 와 smoothstep",
         "\"여기서 원 테두리까지 몇 칸?\"을 각 픽셀이 스스로 재는 거예요. 음수면 안, 양수면 밖.",
         "SDF(Signed Distance Function) = 모양까지의 거리 함수. 원은 length(p - c) - r. smoothstep(a, b, x) 는 x 가 a 이하면 0, b 이상이면 1, 사이는 부드럽게.",
         "거리 d 를 0 또는 1 로 바꿔 \"안/밖\"을 정하고, 경계는 몇 픽셀에 걸쳐 부드럽게 섞어요 (계단 현상 없음).",
         "삼각형을 쓰지 않고 수식만으로 모양을 그리려면 \"이 점이 모양 안인가\"를 픽셀마다 물어야 해요. 장면 1의 무게(안이면 모두 0 이상)와 같은 생각이에요.",
         "inside = 1.0 - smoothstep(r, r + w, d). w 를 0.01 에서 0.1 로 키우면 원 테두리가 흐려져요.");
}

void PlaygroundScene::PanelCodeFlow()
{
    if (!ImGui::CollapsingHeader("이 장면의 DX12 코드 흐름")) return;
    ImGui::Indent();
    ImGui::TextWrapped("[놀이터-①] CreateRootSig(): 루트 시그니처에 루트 상수 8개(b0) 하나만 있어요. 텍스처도 상수 버퍼도 없어요.");
    ImGui::TextWrapped("[놀이터-②] BuildPipeline(): playground.hlsl 의 FullVS, MainPS 로 PSO 하나. 정점 버퍼 없음(InputLayout 비움), 컬링 없음.");
    ImGui::TextWrapped("[놀이터-③] MakeConstants(): 매 프레임 시간, 그림 크기, 마우스를 8개 숫자로 채워요.");
    ImGui::TextWrapped("[놀이터-④] OnRender(): 뷰포트를 그림 영역 크기로 맞추고 DrawInstanced(3) 한 번. 이게 전부예요.");
    ImGui::Unindent();
}

} // namespace

Scene* CreatePlaygroundScene() { return new PlaygroundScene(); }
