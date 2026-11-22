// =====================================================================
//  장면 4: 텍스처 붙이기 - 그림 한 장을 VRAM 에 올리고, 샘플러로 읽기 (학습 로드맵 3단계)
//
//  지금까지는 색을 숫자(상수)로 보냈어요. 이번에는 "그림"을 보내요.
//  그림은 크고(밉맵까지 약 1.3MB), 매 프레임 바뀌지 않으니까 VRAM(Default 힙)에 한 번 올려 두고 계속 읽어요.
//
//  그림이 VRAM 까지 가는 길 (발표의 "RAM -> Upload -> VRAM" 흐름과 같아요):
//    CPU 배열 -> Upload 버퍼(RAM) -> CopyTextureRegion(GPU 가 복사) -> Default 힙 텍스처(VRAM)
//    -> 배리어(복사용 -> 읽기용) -> SRV 카드(주소록에 꽂기) -> 셰이더의 t0
//  그리고 "어떻게 읽을지"는 샘플러 카드(s0)가 정해요: 필터, 주소 모드, 밉맵.
//  패널에서 샘플러를 바꿔 가며 화면이 어떻게 달라지는지 봐요.
//
//  코드에서 Ctrl+F 로 찾기:
//    [텍스처-①] CPU 에서 그림 만들기 (체크무늬 256x256) + 밉맵 8장 직접 만들기
//    [텍스처-②] VRAM 에 빈 텍스처 만들기 (Default 힙, COPY_DEST 상태)
//    [텍스처-③] Upload 버퍼에 그림 쓰기 (GetCopyableFootprints: 줄 간격 256바이트 맞추기)
//    [텍스처-④] 복사 명령 + 배리어 (첫 프레임 명령 목록에), 끝나면 Upload 버퍼 지우기
//    [텍스처-⑤] SRV 카드: "이 텍스처를 이렇게 읽으세요" 를 주소록(SRV 힙)에 꽂기
//    [텍스처-⑥] 샘플러 힙: 읽는 규칙 카드 18장 (필터 3 x 주소 모드 3 x 밉 켬/끔 2)
//    [텍스처-⑦] 루트 시그니처(t0 테이블, s0 테이블, b0 상수) + 그리기
//    [텍스처-⑧] 평면 보기의 네모 = 정점 버퍼(점 4개, 위치 + uv) + 인덱스 버퍼(6개), Default 힙 (게임 방식)
//               바닥 보기는 화면을 덮는 삼각형(정점 버퍼 없음) - 게임의 화면 전체 효과와 같은 방법
// =====================================================================
#include "../scene.h"
#include "../mesh.h"
#include "../shader_file.h"
#include "../ui_help.h"
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <vector>

namespace {

static const UINT kTexSize = 256;   // 그림 크기 (가로 = 세로)
static const UINT kMips    = 9;     // 256, 128, 64, 32, 16, 8, 4, 2, 1 -> 9장
static const UINT kTexCount = 2;    // 0 = 보통 그림, 1 = 밉 레벨 색칠 그림

enum Filter  { kPoint = 0, kLinear = 1, kAniso = 2, kFilterCount = 3 };
enum Address { kWrap = 0, kClamp = 1, kMirror = 2, kAddressCount = 3 };
static const char* kFilterName[kFilterCount]   = { "POINT", "LINEAR", "ANISOTROPIC" };
static const char* kAddressName[kAddressCount] = { "WRAP", "CLAMP", "MIRROR" };
static const UINT  kSamplerCount = 2 * kFilterCount * kAddressCount;   // 18장

// 밉 레벨 색칠: 레벨 1..8 의 색 (레벨 0 은 원래 그림 그대로)
static const float kMipTint[kMips][3] = {
    { 1, 1, 1 }, { 1.0f, 0.25f, 0.25f }, { 1.0f, 0.6f, 0.2f }, { 1.0f, 0.95f, 0.2f }, { 0.3f, 1.0f, 0.3f },
    { 0.2f, 0.9f, 1.0f }, { 0.3f, 0.45f, 1.0f }, { 0.7f, 0.35f, 1.0f }, { 1.0f, 0.3f, 0.85f },
};
static const char* kMipTintName[kMips] = { "원래 그림", "빨강", "주황", "노랑", "초록", "하늘", "파랑", "보라", "분홍" };

// HLSL 의 cbuffer TexConstants 와 순서·크기가 같아야 한다 (루트 상수 8개)
struct TexConstants
{
    float aspect;    // 그림 영역 너비 / 높이
    float uvScale;   // UV 배수
    float view;      // 0 = 평면, 1 = 바닥
    float scroll;    // 바닥 움직인 거리
    float zoom;      // 평면 확대
    float pad[3];
};
static_assert(sizeof(TexConstants) == 32, "float 8개 = 32바이트");

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

// 그림 위에 글자: 뒤에 반투명 검은 띠를 깔아서 무늬 위에서도 읽히게
static void LabelText(ImDrawList* dl, ImVec2 at, ImU32 col, const char* text)
{
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const float pad = ImGui::GetFontSize() * 0.2f;
    dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad * 0.5f), ImVec2(at.x + sz.x + pad, at.y + sz.y + pad * 0.5f), IM_COL32(0, 0, 0, 150), pad);
    dl->AddText(at, col, text);
}

static uint32_t Rgba(int r, int g, int b) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u; }
static int Ch(uint32_t c, int i) { return (int)((c >> (8 * i)) & 0xFF); }

// 그림 한 장 = 밉 레벨마다 픽셀 배열 하나 (RGBA 8비트씩 = 픽셀 하나 4바이트)
struct Image
{
    std::vector<uint32_t> level[kMips];
    UINT Size(UINT m) const { return std::max(1u, kTexSize >> m); }
};

// 텍스처 하나에 딸린 것들
struct Tex
{
    ComPtr<ID3D12Resource>      gpu;      // VRAM (Default 힙) 의 텍스처
    ComPtr<ID3D12Resource>      upload;   // 올리는 동안만 쓰는 Upload 버퍼 (RAM)
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    UINT                        srvIndex = 0;   // 주소록(SRV 힙)에서 몇 번째 칸인지 (패널 표시용)
};

// ===== [텍스처-⑧] 평면 보기의 네모: 정점 하나 = 위치 2개 + uv 2개 = 16바이트. tex.hlsl 의 QuadVertex 와 짝 =====
struct QuadVertex
{
    float position[2];   // POSITION : 네모 모서리 (-1..1, 위가 +1). 화면 크기는 셰이더가 gZoom, gAspect 로 맞춘다
    float uv[2];         // TEXCOORD : 그림 안의 위치 (왼쪽 위 0, 0)
};
static_assert(sizeof(QuadVertex) == 16, "float 4개 = 16바이트");
static const QuadVertex kQuad[4] =
{
    { { -1.0f,  1.0f }, { 0.0f, 0.0f } },   // 0 왼쪽 위
    { {  1.0f,  1.0f }, { 1.0f, 0.0f } },   // 1 오른쪽 위
    { {  1.0f, -1.0f }, { 1.0f, 1.0f } },   // 2 오른쪽 아래
    { { -1.0f, -1.0f }, { 0.0f, 1.0f } },   // 3 왼쪽 아래
};
static const uint16_t kQuadIdx[6] = { 0, 1, 2, 0, 2, 3 };
static const D3D12_INPUT_ELEMENT_DESC kQuadLayout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(QuadVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(QuadVertex, uv),       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

// 올리기 진행 상태 (패널의 "그림이 VRAM 까지 가는 길" 표)
enum UploadState { kCpuOnly = 0, kInUpload = 1, kCopyRecorded = 2, kDone = 3 };

class TextureScene : public Scene
{
public:
    const char* Name() const override { return "장면 4: 텍스처 붙이기 (그림 한 장 VRAM 에 올리기)"; }
    void Init() override;
    void Shutdown() override;
    void OnFrameReused(UINT slot) override;
    void OnPanel() override;
    void OnCanvas(const Canvas& c) override;
    void OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c) override;
    void OnMinimized(const Canvas& c) override;
    bool AutoTest(int frame, int& failures) override;

private:
    ShaderFile                   m_sh;   // lab/shaders/tex.hlsl
    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_pso;       // 바닥 보기: 화면을 덮는 삼각형 (TexVS, 정점 버퍼 없음)
    ComPtr<ID3D12PipelineState>  m_psoQuad;   // 평면 보기: 정점 버퍼 네모 (TexQuadVS) [텍스처-⑧]
    Mesh                         m_quad;      // [텍스처-⑧] 네모 정점 4개 + 인덱스 6개 (Default 힙)
    ComPtr<ID3D12DescriptorHeap> m_samplerHeap;
    UINT                         m_samplerInc = 0;
    Tex                          m_tex[kTexCount];

    // 올리기 정보 (두 텍스처가 크기, 밉 수가 같아서 배치도 같다)
    D3D12_RESOURCE_DESC                m_texDesc{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_fp[kMips] = {};
    UINT                               m_rows[kMips] = {};
    UINT64                             m_rowBytes[kMips] = {};
    UINT64                             m_uploadBytes = 0;   // Upload 버퍼 크기 (한 장)
    UINT64                             m_pixelBytes = 0;    // 진짜 픽셀 바이트 합 (한 장, 밉 포함)
    UINT64                             m_vramBytes = 0;     // VRAM 에서 실제로 차지하는 크기 (한 장)
    int                                m_uploadState = kCpuOnly;
    UINT                               m_copySlot = 0;      // 복사 명령을 기록한 프레임 자원 세트
    uint64_t                           m_copyFrame = 0, m_freeFrame = 0;

    // 설정
    int   m_view = 0;          // 0 평면, 1 바닥
    int   m_filter = kLinear;
    int   m_address = kWrap;
    bool  m_mips = true;
    bool  m_tinted = false;    // 밉 레벨 색칠 그림 쓰기
    float m_uvScale = 1.0f;
    float m_zoom = 1.0f;
    bool  m_move = true;       // 바닥 움직이기
    float m_scroll = 0.0f;
    float m_aspect = 1.0f;     // 그림 영역 너비 / 높이

    void MakeImages(Image img[kTexCount]) const;
    void CreateTextures(const Image img[kTexCount]);
    void WriteUpload(Tex& t, const Image& img);
    void RecordCopies(ID3D12GraphicsCommandList* cl);
    void CreateSrvs();
    void CreateSamplers();
    void CreateRootSig();
    bool BuildPipeline();
    UINT SamplerIndex() const { return (m_mips ? 9u : 0u) + (UINT)m_filter * 3u + (UINT)m_address; }
    void DrawCanvasText(const Canvas& c);

    void PanelView();
    void PanelSampler();
    void PanelTexture();
    void PanelJourney();
    void PanelMipTable();
    void PanelShader();
    void PanelEli5();
    void PanelCodeFlow();

    // ---- 개발용 자동 테스트 (화면의 정해진 점 픽셀을 되읽어 색 확인) ----
    static const UINT kProbes = 5;
    ComPtr<ID3D12Resource> m_readback[kFrameCount];
    int  m_capTag[kFrameCount] = { -1, -1 };
    int  m_testCase = -1, m_testWait = 0, m_gotTag = -1;
    unsigned m_gotRgb[kProbes][3] = {};
    unsigned m_floorRgb[2][3] = {};   // 바닥 먼 곳 색 (밉 켬 / 끔)
    void ProbePixel(UINT i, const Canvas& c, UINT& px, UINT& py) const;
    void CaptureProbes(ID3D12GraphicsCommandList* cl, const Canvas& c);
    void ApplyTestCase(int k);
};

// ---------------------------------------------------------------------
// GPU 자원 (Samples 의 LoadAssets)
// ---------------------------------------------------------------------
void TextureScene::Init()
{
    {
        Image img[kTexCount];
        MakeImages(img);       // [텍스처-①]
        CreateTextures(img);   // [텍스처-②] [텍스처-③]  (CPU 배열은 이 블록이 끝나면 지워져요)
    }
    CreateSrvs();          // [텍스처-⑤]
    CreateSamplers();      // [텍스처-⑥]
    CreateRootSig();       // [텍스처-⑦]
    // [텍스처-⑧] 평면 보기의 네모를 VRAM 에 올린다 (안은 mesh.cpp 의 [정점-②~⑥])
    m_quad = CreateMesh(kQuad, sizeof(QuadVertex), 4, kQuadIdx, 6, MeshHeap::Default, L"texture quad");
    m_sh.Open("tex.hlsl");
    m_sh.editorOpen = false;   // 이 장면의 주인공은 패널(샘플러 바꿔 보기). 셰이더 창은 F2 로
    BuildPipeline();
    if (GetEnvironmentVariableA("LAB_AUTOTEST", nullptr, 0) > 0)
        for (UINT f = 0; f < kFrameCount; f++)
            m_readback[f] = CreateBuffer(D3D12_HEAP_TYPE_READBACK, 512 * kProbes, D3D12_RESOURCE_STATE_COPY_DEST);
}

void TextureScene::Shutdown()
{
    for (Tex& t : m_tex) if (t.srvCpu.ptr) FreeSrv(t.srvCpu);
}

// ===== [텍스처-①] CPU 에서 그림 만들기 =====
// 8x8 체크무늬(크림색, 청록색). 칸 경계에 어두운 선 1픽셀, 가운데에 어두운 동그라미.
// 네 모서리 칸은 색을 달리 칠해서 방향을 알 수 있게: 왼쪽 위 빨강, 오른쪽 위 초록, 왼쪽 아래 노랑, 오른쪽 아래 파랑.
void TextureScene::MakeImages(Image img[kTexCount]) const
{
    Image& a = img[0];
    const uint32_t cream = Rgba(238, 228, 200), teal = Rgba(40, 128, 140), line = Rgba(25, 40, 50);
    const uint32_t corner[4] = { Rgba(225, 45, 45), Rgba(45, 200, 70), Rgba(235, 210, 40), Rgba(50, 90, 230) };
    a.level[0].resize(kTexSize * kTexSize);
    for (UINT y = 0; y < kTexSize; y++)
        for (UINT x = 0; x < kTexSize; x++)
        {
            const UINT cx = x / 32, cy = y / 32;   // 몇 번째 칸인지 (0..7)
            uint32_t c = ((cx + cy) & 1) ? teal : cream;
            if (cx == 0 && cy == 0) c = corner[0];
            if (cx == 7 && cy == 0) c = corner[1];
            if (cx == 0 && cy == 7) c = corner[2];
            if (cx == 7 && cy == 7) c = corner[3];
            if (x % 32 == 0 || y % 32 == 0) c = line;   // 칸 경계선 (1픽셀: 멀리서 반짝이는 원인)
            const float dx = x + 0.5f - 128.0f, dy = y + 0.5f - 128.0f;
            if (std::fabs(std::sqrt(dx * dx + dy * dy) - 72.0f) < 1.5f) c = line;   // 동그라미 (계단이 잘 보여요)
            a.level[0][y * kTexSize + x] = c;
        }

    // 밉맵 직접 만들기: 윗 레벨의 2x2 픽셀 4개 평균 = 아래 레벨 픽셀 1개 (박스 필터)
    // DX11 에는 GenerateMips 가 있었지만 DX12 에는 없어요. CPU 나 컴퓨트 셰이더로 직접 만들어요.
    for (UINT m = 1; m < kMips; m++)
    {
        const UINT s = a.Size(m), ps = a.Size(m - 1);
        a.level[m].resize(s * s);
        for (UINT y = 0; y < s; y++)
            for (UINT x = 0; x < s; x++)
            {
                const uint32_t p[4] = { a.level[m - 1][(2 * y) * ps + 2 * x], a.level[m - 1][(2 * y) * ps + 2 * x + 1],
                                        a.level[m - 1][(2 * y + 1) * ps + 2 * x], a.level[m - 1][(2 * y + 1) * ps + 2 * x + 1] };
                int ch[3];
                for (int i = 0; i < 3; i++) ch[i] = (Ch(p[0], i) + Ch(p[1], i) + Ch(p[2], i) + Ch(p[3], i) + 2) / 4;
                a.level[m][y * s + x] = Rgba(ch[0], ch[1], ch[2]);
            }
    }

    // 밉 레벨 색칠 그림: 레벨 0 은 같고, 레벨 1..8 은 밝기만 남기고 레벨 색을 입힌다
    // -> 화면에서 GPU 가 어느 밉을 골랐는지 색으로 보여요
    Image& b = img[1];
    b.level[0] = a.level[0];
    for (UINT m = 1; m < kMips; m++)
    {
        b.level[m].resize(a.level[m].size());
        for (size_t i = 0; i < a.level[m].size(); i++)
        {
            const uint32_t p = a.level[m][i];
            const float lum = (0.3f * Ch(p, 0) + 0.59f * Ch(p, 1) + 0.11f * Ch(p, 2)) / 255.0f;
            const float k = 0.35f + 0.65f * lum;
            b.level[m][i] = Rgba((int)(255 * k * kMipTint[m][0]), (int)(255 * k * kMipTint[m][1]), (int)(255 * k * kMipTint[m][2]));
        }
    }
}

// ===== [텍스처-②] VRAM 에 빈 텍스처 만들기 + [텍스처-③] Upload 버퍼에 그림 쓰기 =====
void TextureScene::CreateTextures(const Image img[kTexCount])
{
    // 텍스처 설명서: 256x256, RGBA 8비트, 밉 9장
    m_texDesc = {};
    m_texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    m_texDesc.Width = kTexSize;
    m_texDesc.Height = kTexSize;
    m_texDesc.DepthOrArraySize = 1;
    m_texDesc.MipLevels = kMips;
    m_texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_texDesc.SampleDesc.Count = 1;
    m_texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;   // 픽셀을 VRAM 에 어떻게 늘어놓을지는 GPU 마음대로 (빨리 읽히는 모양으로)

    // Upload 버퍼 안에 밉 9장을 어떻게 늘어놓을지 D3D12 에게 물어본다
    // (줄 간격 RowPitch 는 256바이트의 배수, 각 밉의 시작 위치 Offset 은 512바이트의 배수여야 해요)
    g_dx.device->GetCopyableFootprints(&m_texDesc, 0, kMips, 0, m_fp, m_rows, m_rowBytes, &m_uploadBytes);
    m_pixelBytes = 0;
    for (UINT m = 0; m < kMips; m++) m_pixelBytes += (UINT64)img[0].Size(m) * img[0].Size(m) * 4;
    m_vramBytes = g_dx.device->GetResourceAllocationInfo(0, 1, &m_texDesc).SizeInBytes;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;   // VRAM. CPU 는 못 건드리고 GPU 만 빨리 읽고 써요
    for (UINT t = 0; t < kTexCount; t++)
    {
        // 처음 상태 COPY_DEST: "곧 복사로 채워질 자리"
        HR(g_dx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &m_texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&m_tex[t].gpu)));
        WriteUpload(m_tex[t], img[t]);
    }
    m_uploadState = kInUpload;
}

// [텍스처-③] Upload 버퍼(RAM)를 만들고 밉마다 한 줄씩 복사. 줄 간격은 그림 너비가 아니라 RowPitch!
void TextureScene::WriteUpload(Tex& t, const Image& img)
{
    t.upload = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, m_uploadBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* dst = nullptr;
    D3D12_RANGE none{ 0, 0 };
    HR(t.upload->Map(0, &none, (void**)&dst));
    for (UINT m = 0; m < kMips; m++)
    {
        const UINT w = img.Size(m);
        for (UINT y = 0; y < m_rows[m]; y++)
            memcpy(dst + m_fp[m].Offset + (size_t)y * m_fp[m].Footprint.RowPitch,   // Upload 버퍼: 줄마다 RowPitch 만큼 떨어져 있음
                   img.level[m].data() + (size_t)y * w,                            // CPU 배열: 줄마다 너비 x 4바이트
                   (size_t)m_rowBytes[m]);                                         // 한 줄의 진짜 픽셀 바이트
    }
    t.upload->Unmap(0, nullptr);
}

// ===== [텍스처-④] 복사 명령 + 배리어. 첫 프레임 명령 목록에 기록 =====
// CopyTextureRegion 은 "GPU 야, Upload 버퍼의 이 부분을 텍스처의 밉 m 에 복사해" 라는 주문이에요.
// CPU 가 직접 VRAM 에 쓰는 게 아니라 GPU 가 복사해요. 그래서 명령 목록에 넣고, GPU 가 실행해야 끝나요.
void TextureScene::RecordCopies(ID3D12GraphicsCommandList* cl)
{
    D3D12_RESOURCE_BARRIER bars[kTexCount];
    for (UINT t = 0; t < kTexCount; t++)
    {
        for (UINT m = 0; m < kMips; m++)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = m_tex[t].gpu.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = m;               // 밉 m 장
            src.pResource = m_tex[t].upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = m_fp[m];          // Upload 버퍼의 어디부터, 줄 간격 얼마
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        // 배리어: "복사 받는 중" -> "픽셀 셰이더가 읽는 중". 이걸 안 하면 GPU 가 복사가 끝나기 전에 읽을 수 있어요
        bars[t] = Transition(m_tex[t].gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    cl->ResourceBarrier(kTexCount, bars);
    m_uploadState = kCopyRecorded;
    m_copySlot = g_dx.frameIndex;
    m_copyFrame = g_dx.frameNo;
}

// 같은 프레임 자원 세트가 다시 돌아왔다 = GPU 가 그 프레임(복사 포함)을 다 끝냈다 -> Upload 버퍼를 지워도 된다
void TextureScene::OnFrameReused(UINT slot)
{
    if (m_uploadState == kCopyRecorded && slot == m_copySlot && g_dx.frameNo > m_copyFrame)
    {
        for (Tex& t : m_tex) t.upload.Reset();   // RAM 반납. 이제 그림은 VRAM 에만 있어요
        m_uploadState = kDone;
        m_freeFrame = g_dx.frameNo;
    }

    // 자동 테스트 되읽기
    if (!m_readback[0] || m_capTag[slot] < 0) return;
    uint8_t* p = nullptr;
    D3D12_RANGE r{ 0, 512 * kProbes };
    if (FAILED(m_readback[slot]->Map(0, &r, (void**)&p))) return;
    for (UINT i = 0; i < kProbes; i++)
        for (int j = 0; j < 3; j++) m_gotRgb[i][j] = p[512 * i + j];
    D3D12_RANGE none{ 0, 0 };
    m_readback[slot]->Unmap(0, &none);
    m_gotTag = m_capTag[slot];
    m_capTag[slot] = -1;
}

// ===== [텍스처-⑤] SRV 카드: "VRAM 의 이 텍스처를, RGBA8 2D 그림으로, 밉 9장 다 보면서 읽어요" =====
void TextureScene::CreateSrvs()
{
    const SIZE_T heapStart = g_dx.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    for (Tex& t : m_tex)
    {
        AllocSrv(&t.srvCpu, &t.srvGpu);
        t.srvIndex = (UINT)((t.srvCpu.ptr - heapStart) / g_dx.srvInc);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = m_texDesc.Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;   // R->R, G->G, B->B, A->A 그대로
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels = kMips;
        g_dx.device->CreateShaderResourceView(t.gpu.Get(), &sd, t.srvCpu);
    }
}

// ===== [텍스처-⑥] 샘플러 힙: 읽는 규칙 카드 18장을 미리 다 만들어 둔다 =====
// 카드 번호 = 밉(0 끔, 9 켬) + 필터 x 3 + 주소 모드.  패널에서 바꾸면 "몇 번 카드를 쓸지"만 바뀌어요.
void TextureScene::CreateSamplers()
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;          // 샘플러 카드는 CBV/SRV 와 다른 주소록에 따로 꽂아요
    hd.NumDescriptors = kSamplerCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;  // 셰이더가 볼 수 있는 주소록
    HR(g_dx.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_samplerHeap)));
    m_samplerInc = g_dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    const D3D12_FILTER filters[kFilterCount] = { D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_ANISOTROPIC };
    const D3D12_TEXTURE_ADDRESS_MODE modes[kAddressCount] = { D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR };
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT mip = 0; mip < 2; mip++)
        for (UINT f = 0; f < kFilterCount; f++)
            for (UINT a = 0; a < kAddressCount; a++)
            {
                D3D12_SAMPLER_DESC sd{};
                sd.Filter = filters[f];
                sd.AddressU = sd.AddressV = sd.AddressW = modes[a];
                sd.MaxAnisotropy = (f == kAniso) ? 16 : 1;          // 비스듬한 방향으로 최대 16번 더 읽기
                sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;    // 그림자 비교용. 여기선 안 씀
                sd.MinLOD = 0.0f;
                sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : 0.0f;         // 0 이면 밉 0장(원본)만 쓰기 = 밉맵 끄기
                const UINT index = mip * 9 + f * 3 + a;
                D3D12_CPU_DESCRIPTOR_HANDLE at{ h.ptr + (SIZE_T)index * m_samplerInc };
                g_dx.device->CreateSampler(&sd, at);
            }
}

// ===== [텍스처-⑦] 루트 시그니처: 가방 칸 3개 =====
//   칸 0: 테이블 -> SRV 카드 1장 (t0)     칸 1: 테이블 -> 샘플러 카드 1장 (s0)     칸 2: 루트 상수 8개 (b0)
// 텍스처(SRV)는 루트 상수나 루트 CBV 처럼 가방에 직접 못 넣어요. 꼭 디스크립터 테이블(장면 3의 방법 C)로 넘겨요.
void TextureScene::CreateRootSig()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;   // t0
    D3D12_DESCRIPTOR_RANGE sampRange{};
    sampRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampRange.NumDescriptors = 1;
    sampRange.BaseShaderRegister = 0;  // s0

    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 1;
    p[0].DescriptorTable.pDescriptorRanges = &srvRange;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;   // 그림은 픽셀 셰이더만 읽어요
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &sampRange;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[2].Constants.ShaderRegister = 0;   // b0
    p[2].Constants.Num32BitValues = sizeof(TexConstants) / 4;
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3; rs.pParameters = p;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;   // 평면 보기가 정점 버퍼를 쓴다 [텍스처-⑧]
    m_rootSig = CreateRootSignature(rs);
}

bool TextureScene::BuildPipeline()
{
    ComPtr<ID3DBlob> vs, vsQuad, ps;
    std::string err;
    bool ok = TryCompile(m_sh, "TexVS", "vs_5_0", vs, err);
    ok &= TryCompile(m_sh, "TexQuadVS", "vs_5_0", vsQuad, err);
    ok &= TryCompile(m_sh, "TexPS", "ps_5_0", ps, err);
    if (!ok) { m_sh.error = err; return false; }   // 문법 에러: 지금 쓰던 PSO 를 그대로 둔다

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_rootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { nullptr, 0 };   // 바닥 보기: 정점 버퍼 없음 (SV_VertexID)
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = kBackBufferFormat;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso, psoQuad;
    const bool okFloor = SUCCEEDED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    // [텍스처-⑧] 평면 보기: 같은 PS, VS 만 TexQuadVS + 입력 레이아웃 (정점 버퍼 16바이트)
    pd.VS = { vsQuad->GetBufferPointer(), vsQuad->GetBufferSize() };
    pd.InputLayout = { kQuadLayout, (UINT)std::size(kQuadLayout) };
    const bool okQuad = SUCCEEDED(g_dx.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoQuad)));
    if (!okFloor || !okQuad)
    {
        m_sh.error = "컴파일은 됐지만 PSO 를 못 만들었어요.\n"
                     "흔한 이유: t1, s1, b1 처럼 루트 시그니처(가방)에 없는 레지스터를 씀. 이 장면의 가방에는 t0, s0, b0 만 있어요.\n"
                     "또는 QuadVertex 의 시맨틱(POSITION, TEXCOORD)이 C++ 입력 레이아웃과 다름.\n";
        return false;
    }
    if (m_pso) WaitIdle();   // GPU 가 옛 PSO 를 다 쓴 뒤에 바꾼다
    m_pso = pso;
    m_psoQuad = psoQuad;
    m_sh.error.clear();
    m_sh.buildCount++;
    return true;
}

// ---------------------------------------------------------------------
// 왼쪽 그림 영역
// ---------------------------------------------------------------------
void TextureScene::OnCanvas(const Canvas& c)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (m_view == 1 && m_move) m_scroll += io.DeltaTime * 0.6f;
    m_aspect = c.w / std::max(1.0f, c.h);
    // 평면 보기: 그림 영역 위에서 마우스 휠 = 확대/축소
    const bool inCanvas = io.MousePos.x >= c.x && io.MousePos.x < c.x + c.w && io.MousePos.y >= c.y && io.MousePos.y < c.y + c.h;
    if (m_view == 0 && inCanvas && io.MouseWheel != 0.0f && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
        m_zoom = std::clamp(m_zoom * std::pow(1.15f, io.MouseWheel), 0.05f, 16.0f);
    DrawCanvasText(c);
    DrawShaderEditor(m_sh, c);
}

void TextureScene::DrawCanvasText(const Canvas& c)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    const float fs = ImGui::GetFontSize();
    char line[256];
    snprintf(line, sizeof line, "지금: %s 보기 · 필터 %s · 주소 %s · 밉맵 %s%s · F2 셰이더 창",
             m_view ? "바닥" : "평면", kFilterName[m_filter], kAddressName[m_address], m_mips ? "켬" : "끔",
             m_tinted ? " · 밉 레벨 색칠" : "");
    LabelText(dl, ImVec2(c.x + fs * 0.6f, c.y + fs * 0.4f), IM_COL32(255, 255, 255, 220), line);
    if (!m_pso)
        LabelText(dl, ImVec2(c.x + fs * 0.6f, c.y + fs * 1.8f), IM_COL32(255, 120, 120, 255),
                    "PSO 가 없어서 아무것도 안 그려요. 셰이더 창의 빨간 글씨를 보세요.");

    if (m_view == 0)
    {
        // 네모의 화면 위치 (셰이더 TexQuadVS 와 같은 계산)
        const float half = 0.8f / m_zoom;
        auto toScreen = [&](float bx, float by) {
            const float qx = (bx - 0.5f) * 2.0f * half, qy = (by - 0.5f) * 2.0f * half;
            return ImVec2(c.x + (qx / m_aspect * 0.5f + 0.5f) * c.w, c.y + (qy * 0.5f + 0.5f) * c.h);
        };
        const ImVec2 tl = toScreen(0, 0), br = toScreen(1, 1);
        // 그림 한 장(uv 0..1) 자리마다 옅은 선 (UV 배수 2 이상일 때)
        for (int i = 1; i < (int)std::ceil(m_uvScale); i++)
        {
            const ImVec2 a = toScreen(i / m_uvScale, 0), b = toScreen(i / m_uvScale, 1);
            const ImVec2 d = toScreen(0, i / m_uvScale), e = toScreen(1, i / m_uvScale);
            dl->AddLine(a, b, IM_COL32(255, 255, 255, 90), 1.0f);
            dl->AddLine(d, e, IM_COL32(255, 255, 255, 90), 1.0f);
        }
        if (tl.x > c.x && tl.y > c.y + fs * 2.0f)
            LabelText(dl, ImVec2(tl.x, tl.y - fs * 1.3f), IM_COL32(255, 230, 120, 255), "uv (0, 0)");
        if (br.x < c.x + c.w - fs * 5 && br.y < c.y + c.h - fs * 1.5f)
        {
            snprintf(line, sizeof line, "uv (%.2g, %.2g)", m_uvScale, m_uvScale);
            LabelText(dl, ImVec2(br.x - fs * 4.0f, br.y + fs * 0.3f), IM_COL32(255, 230, 120, 255), line);
        }
    }
    else
    {
        const float horizonY = c.y + (1.0f - (0.35f * 0.5f + 0.5f)) * c.h;
        LabelText(dl, ImVec2(c.x + fs * 0.6f, horizonY + fs * 0.3f), IM_COL32(255, 255, 255, 200),
                    "먼 바닥: 칸 하나가 픽셀보다 작아요 -> 밉맵이 필요한 곳");
        LabelText(dl, ImVec2(c.x + fs * 0.6f, c.y + c.h - fs * 1.6f), IM_COL32(255, 255, 255, 200),
                    "가까운 바닥: 칸 하나가 픽셀 여러 개 -> 필터(POINT/LINEAR)가 보이는 곳");
    }
}

void TextureScene::OnMinimized(const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipeline();
    DrawShaderEditor(m_sh, c);
}

// ---------------------------------------------------------------------
// GPU 명령 기록 (Samples 의 PopulateCommandList)
// ---------------------------------------------------------------------
void TextureScene::OnRender(ID3D12GraphicsCommandList* cl, const Canvas& c)
{
    if (m_sh.NeedsRebuild()) BuildPipeline();      // .hlsl 을 고쳤으면 다시 컴파일 (핫 리로드)
    if (m_uploadState == kInUpload) RecordCopies(cl);   // [텍스처-④] 첫 프레임에 한 번만

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferRtv();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, c.w, c.h, 0, 1 };
    D3D12_RECT sc{ 0, 0, (LONG)c.w, (LONG)c.h };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12PipelineState* pso = m_view == 0 ? m_psoQuad.Get() : m_pso.Get();
    if (pso)
    {
        // 주소록 두 권을 "이걸 보세요" 하고 펼쳐 둔다: CBV/SRV/UAV 주소록 + 샘플러 주소록 (종류마다 한 권씩만)
        ID3D12DescriptorHeap* heaps[] = { g_dx.srvHeap.Get(), m_samplerHeap.Get() };
        cl->SetDescriptorHeaps(2, heaps);
        cl->SetGraphicsRootSignature(m_rootSig.Get());
        cl->SetPipelineState(pso);

        const Tex& t = m_tex[m_tinted ? 1 : 0];
        D3D12_GPU_DESCRIPTOR_HANDLE samp = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        samp.ptr += (UINT64)SamplerIndex() * m_samplerInc;
        TexConstants k{};
        k.aspect = m_aspect; k.uvScale = m_uvScale; k.view = (float)m_view; k.scroll = m_scroll; k.zoom = m_zoom;

        cl->SetGraphicsRootDescriptorTable(0, t.srvGpu);   // 칸 0 (t0): 주소록의 SRV 카드 위치
        cl->SetGraphicsRootDescriptorTable(1, samp);       // 칸 1 (s0): 샘플러 주소록의 카드 위치
        cl->SetGraphicsRoot32BitConstants(2, sizeof(TexConstants) / 4, &k, 0);   // 칸 2 (b0)
        if (m_view == 0) m_quad.Draw(cl);                  // 평면: 정점 버퍼 네모 (DrawIndexedInstanced(6)) [텍스처-⑧]
        else             cl->DrawInstanced(3, 1, 0, 0);    // 바닥: 화면을 덮는 삼각형 하나
    }

    if (m_readback[0]) CaptureProbes(cl, c);   // 자동 테스트 때만
}

// ---------------------------------------------------------------------
// 오른쪽 패널
// ---------------------------------------------------------------------
void TextureScene::OnPanel()
{
    ImGui::TextWrapped("그림 한 장(256x256 체크무늬)을 VRAM 에 올려 두고, 셰이더가 \"샘플러\"라는 규칙으로 읽어요. "
                       "샘플러를 바꾸면 같은 그림이 다르게 보여요. 그림은 그대로, 읽는 방법만 바뀌는 거예요.");
    PanelView();
    PanelSampler();
    PanelTexture();
    PanelShader();
    PanelJourney();
    PanelMipTable();
    PanelEli5();
    PanelCodeFlow();
}

void TextureScene::PanelView()
{
    ImGui::SeparatorText("보기");
    ImGui::RadioButton("평면", &m_view, 0);
    ImGui::SameLine();
    ImGui::RadioButton("바닥 (멀리까지)", &m_view, 1);
    Help("평면: 화면 가운데 네모에 그림을 붙여요. 확대하면 필터, UV 배수를 키우면 주소 모드가 보여요.\n"
         "바닥: 그림을 끝없는 바닥에 깔아요. 멀어질수록 그림이 작아져서 밉맵이 왜 필요한지 보여요.\n"
         "그리는 방법도 달라요 (둘 다 실제 게임에서 쓰는 방법):\n"
         "  평면 = 네모를 정점 버퍼(점 4개, 점마다 위치 + uv) + 인덱스 버퍼(6개)로 그려요. 그림 붙은 판, 모델 표면이 이렇게 와요. [텍스처-⑧]\n"
         "  바닥 = 화면을 덮는 삼각형 하나(정점 버퍼 없음)에 픽셀 셰이더가 픽셀마다 uv 를 계산해요. 게임의 화면 전체 효과(후처리, 하늘)와 같은 방법.");
    if (m_view == 0)
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::SliderFloat("확대", &m_zoom, 0.05f, 16.0f, "%.2f배", ImGuiSliderFlags_Logarithmic);
        Help("그림 영역 위에서 마우스 휠로도 바꿀 수 있어요.\n"
             "크게 (4배 이상): 그림 픽셀(텍셀) 하나가 화면 픽셀 여러 개를 덮어요 = 확대(magnification). POINT 와 LINEAR 차이가 잘 보여요.\n"
             "작게 (0.2배 이하): 화면 픽셀 하나에 텍셀 여러 개가 들어가요 = 축소(minification). 밉맵을 끄면 지글거려요.");
        ImGui::SameLine();
        if (ImGui::SmallButton("1배")) m_zoom = 1.0f;
    }
    else
    {
        ImGui::Checkbox("바닥 움직이기", &m_move);
        Help("움직일 때 먼 바닥의 지글거림(반짝임)이 잘 보여요. 밉맵을 끄고 켜서 비교해 보세요.");
    }
}

void TextureScene::PanelSampler()
{
    ImGui::SeparatorText("샘플러 (그림을 읽는 규칙)");
    ImGui::TextUnformatted("필터");
    ImGui::SameLine();
    ImGui::RadioButton("POINT", &m_filter, kPoint); ImGui::SameLine();
    ImGui::RadioButton("LINEAR", &m_filter, kLinear); ImGui::SameLine();
    ImGui::RadioButton("ANISO", &m_filter, kAniso);
    Help("uv 가 텍셀과 텍셀 사이를 가리킬 때 어떤 색을 줄지.\n"
         "POINT: 가장 가까운 텍셀 하나의 색 그대로. 확대하면 네모난 계단(픽셀 아트처럼)이 보여요.\n"
         "LINEAR: 둘레 텍셀 4개를 거리만큼 섞어요. 확대하면 부드럽지만 흐릿해요. 밉을 켜면 밉 두 장 사이도 섞어요(트라이리니어).\n"
         "ANISOTROPIC(이방성): 바닥처럼 비스듬히 누운 면에서, 길쭉한 방향으로 여러 번(최대 16번) 더 읽어요. 먼 바닥이 덜 뭉개져요.");
    ImGui::TextUnformatted("주소");
    ImGui::SameLine();
    ImGui::RadioButton("WRAP", &m_address, kWrap); ImGui::SameLine();
    ImGui::RadioButton("CLAMP", &m_address, kClamp); ImGui::SameLine();
    ImGui::RadioButton("MIRROR", &m_address, kMirror);
    Help("uv 가 0..1 밖일 때 어떻게 할지. UV 배수를 2 이상으로 해야 보여요.\n"
         "WRAP(반복): 1.3 -> 0.3. 타일처럼 반복해요. 바닥, 벽돌 벽에 써요.\n"
         "CLAMP(끝 고정): 1.3 -> 1.0. 맨 가장자리 텍셀이 쭉 늘어나요. 반복하면 안 되는 그림(UI, 하늘 가장자리)에 써요.\n"
         "MIRROR(거울): 1.3 -> 0.7. 한 장씩 뒤집어 가며 반복해요. 이음매가 안 보여요.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::SliderFloat("UV 배수", &m_uvScale, 1.0f, 4.0f, "%.1f");
    Help("셰이더에서 uv 에 이 값을 곱해요. 3 이면 네모 안 uv 가 0..3 이 돼서, 그림 3x3 장 자리가 생겨요.\n"
         "0..1 밖의 자리를 어떻게 채우는지가 바로 주소 모드예요.");
    ImGui::Checkbox("밉맵 쓰기", &m_mips);
    Help("끄면 샘플러의 MaxLOD 를 0 으로 해서 밉 0번(원래 256x256)만 읽어요.\n"
         "그림이 작게 보이는 곳(먼 바닥, 많이 축소한 평면)에서 화면 픽셀 하나가 텍셀 수십 개를 덮는데, 그중 하나만 골라 읽으니까 "
         "움직일 때마다 고르는 텍셀이 바뀌어서 지글거려요(앨리어싱). 켜면 미리 줄여 둔 그림(밉)에서 읽어서 차분해요.");
    ImGui::Text("지금 쓰는 샘플러 카드: 샘플러 주소록 #%u (18장 중)", SamplerIndex());
    AboutHelp("샘플러 카드 18장(필터 3 x 주소 3 x 밉 켬/끔 2)을 처음에 다 만들어 두고, 버튼을 누르면 \"몇 번 카드를 쓸지\"만 바꿔요.\n"
              "카드 번호 = (밉 켬이면 9) + 필터 x 3 + 주소.\n"
              "카드를 매번 새로 만들지 않는 이유: 그리는 중인 GPU 가 아직 옛 카드를 읽고 있을 수 있어서 (장면 3 프레임 세트와 같은 문제).");
}

void TextureScene::PanelTexture()
{
    ImGui::SeparatorText("그림");
    ImGui::Checkbox("밉 레벨 색칠", &m_tinted);
    Help("다른 텍스처(SRV 카드)로 바꿔요. 이 그림은 밉 0 은 원래 그림이고, 밉 1..8 은 레벨마다 다른 색을 입혔어요.\n"
         "그래서 화면 색을 보면 GPU 가 그 자리에서 몇 번 밉을 골랐는지 알 수 있어요.\n"
         "평면을 축소해 보거나, 바닥 보기에서 켜 보세요. 가까운 곳은 원래 그림, 멀어질수록 빨강 -> 주황 -> 노랑 ... 순서로 바뀌어요.\n"
         "LINEAR 는 두 밉 사이를 섞어서 색이 부드럽게 넘어가고, POINT 는 경계가 딱 끊어져요.");
    if (m_tinted)
    {
        for (UINT m = 0; m < kMips; m++)
        {
            if (m) ImGui::SameLine(0, 2);
            const float k = (m == 0) ? 0.9f : 1.0f;
            ImGui::ColorButton(kMipTintName[m], ImVec4(kMipTint[m][0] * k, kMipTint[m][1] * k, kMipTint[m][2] * k, 1), 0,
                               ImVec2(ImGui::GetFontSize() * 1.6f, ImGui::GetFontSize()));
        }
        ImGui::TextDisabled("왼쪽부터 밉 0 (원래 그림), 1, 2, ... 8. 마우스를 올리면 이름");
    }
    ImGui::Text("SRV 카드: 주소록 #%u (보통), #%u (밉 색칠)", m_tex[0].srvIndex, m_tex[1].srvIndex);
}

void TextureScene::PanelShader()
{
    ImGui::SeparatorText("셰이더 (tex.hlsl)");
    if (ImGui::Button(m_sh.editorOpen ? "셰이더 창 닫기 (F2)" : "셰이더 창 열기 (F2)")) m_sh.editorOpen = !m_sh.editorOpen;
    ImGui::SameLine();
    if (ImGui::Button("다시 컴파일 (F5)")) m_sh.RequestRebuild();
    ImGui::SameLine();
    if (m_pso && m_sh.error.empty()) ImGui::TextColored(ImVec4(0.55f, 1, 0.55f, 1), "됨");
    else                             ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "에러");
    ImGui::TextDisabled("파일 맨 위 주석에 셰이더 실험 ①~③ 이 있어요");
}

static void Bytes(UINT64 b)
{
    if (b >= 1024 * 1024) ImGui::Text("%.2f MB", b / (1024.0 * 1024.0));
    else if (b >= 1024)   ImGui::Text("%.1f KB", b / 1024.0);
    else                  ImGui::Text("%llu B", (unsigned long long)b);
}

// 그림이 VRAM 까지 가는 길 (진행 상태)
void TextureScene::PanelJourney()
{
    ImGui::SeparatorText("그림이 VRAM 까지 가는 길");
    AboutHelp("발표의 \"RAM -> Upload -> VRAM\" 길을 텍스처로 한 번 걸어 봐요. 크기는 그림 한 장 기준 (이 장면은 2장).\n"
              "Upload 버퍼는 \"택배 상자\"예요. 그림을 VRAM 에 옮기고 나면 필요 없어서, GPU 가 복사를 끝낸 걸 확인한 뒤(같은 프레임 세트가 돌아왔을 때) 지워요.\n"
              "Upload 버퍼에서 바로 읽으면 안 되나? 읽을 수는 있지만(버퍼만, 텍스처 모양은 안 돼요), 매 픽셀 PCIe 너머 RAM 을 읽어서 느려요. "
              "한 번 VRAM 에 옮겨 두면 매 프레임 빠르게 읽어요.");
    if (!ImGui::BeginTable("journey", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableSetupColumn("어디서 무엇을", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("크기", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("상태", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableHeadersRow();
    const ImVec4 ok(0.55f, 1, 0.55f, 1), wait(1, 0.85f, 0.4f, 1), gone(0.6f, 0.6f, 0.6f, 1);
    char buf[96];
    auto row = [&](int n, const char* what, UINT64 size, const ImVec4& col, const char* state) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", n);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", what);
        ImGui::TableNextColumn(); if (size) Bytes(size); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, col); ImGui::TextWrapped("%s", state); ImGui::PopStyleColor();
    };
    row(1, "CPU 배열에 그림 만들기 (밉 9장, 박스 필터)", m_pixelBytes, gone, "Upload 에 옮기고 지움");
    row(2, "Upload 버퍼(RAM)에 줄마다 복사 (RowPitch 간격)", m_uploadBytes,
        m_uploadState == kDone ? gone : ok, m_uploadState == kDone ? "지움 (RAM 반납)" : "씀");
    snprintf(buf, sizeof buf, "프레임 %llu 에 기록", (unsigned long long)m_copyFrame);
    row(3, "CopyTextureRegion x 9 (밉마다, GPU 가 복사)", 0, m_uploadState >= kCopyRecorded ? ok : wait,
        m_uploadState >= kCopyRecorded ? buf : "기다리는 중");
    row(4, "Default 힙 텍스처 (VRAM)", m_vramBytes, ok, "있음");
    row(5, "배리어 COPY_DEST -> PIXEL_SHADER_RESOURCE", 0, m_uploadState >= kCopyRecorded ? ok : wait,
        m_uploadState >= kCopyRecorded ? "복사 바로 뒤에 기록" : "기다리는 중");
    row(6, "SRV 카드를 주소록에 꽂기 -> 셰이더의 t0", 0, ok, "있음");
    snprintf(buf, sizeof buf, "프레임 %llu 에 지움", (unsigned long long)m_freeFrame);
    row(7, "GPU 복사 끝 확인 -> Upload 버퍼 지우기", 0, m_uploadState == kDone ? ok : wait,
        m_uploadState == kDone ? buf : "GPU 가 복사하는 중");
    ImGui::EndTable();
    ImGui::TextDisabled("VRAM 크기는 GetResourceAllocationInfo 값 (드라이버가 정렬해서 조금 더 커요)");
}

// 밉 레벨 표: 크기, 바이트, 줄 간격, 위치
void TextureScene::PanelMipTable()
{
    ImGui::SeparatorText("밉 레벨 (Upload 버퍼 배치)");
    AboutHelp("밉 9장이 Upload 버퍼 안에 어떻게 놓이는지 (GetCopyableFootprints 가 알려 준 값).\n"
              "줄 간격(RowPitch): 한 줄의 진짜 픽셀은 너비 x 4바이트인데, 복사용 버퍼에서는 줄마다 256바이트의 배수로 맞춰야 해요. "
              "그래서 32x32 밉은 한 줄 128바이트인데 256바이트 간격이에요(나머지는 빈칸).\n"
              "시작 위치(Offset): 밉마다 512바이트의 배수에서 시작해요.\n"
              "밉 1..8 을 다 합쳐도 원래 그림의 1/3 정도예요 (1/4 + 1/16 + ... = 1/3). 그래서 밉맵은 메모리 +33% 로 지글거림을 없애는 거래예요.");
    if (!ImGui::BeginTable("mips", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("밉");
    ImGui::TableSetupColumn("크기");
    ImGui::TableSetupColumn("픽셀 바이트");
    ImGui::TableSetupColumn("줄 간격");
    ImGui::TableSetupColumn("시작 위치");
    ImGui::TableHeadersRow();
    for (UINT m = 0; m < kMips; m++)
    {
        const UINT s = std::max(1u, kTexSize >> m);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (m_tinted && m)
        {
            ImGui::ColorButton("##t", ImVec4(kMipTint[m][0], kMipTint[m][1], kMipTint[m][2], 1), ImGuiColorEditFlags_NoTooltip,
                               ImVec2(ImGui::GetFontSize() * 0.8f, ImGui::GetFontSize() * 0.8f));
            ImGui::SameLine();
        }
        ImGui::Text("%u", m);
        ImGui::TableNextColumn(); ImGui::Text("%ux%u", s, s);
        ImGui::TableNextColumn(); ImGui::Text("%u", s * s * 4);
        ImGui::TableNextColumn();
        const UINT pitch = m_fp[m].Footprint.RowPitch;
        if (pitch > s * 4) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "%u (%u+빈칸)", pitch, s * 4);
        else               ImGui::Text("%u", pitch);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)m_fp[m].Offset);
    }
    ImGui::EndTable();
    const UINT64 base = (UINT64)kTexSize * kTexSize * 4;
    ImGui::Text("밉 0 만: %llu B   밉 9장: %llu B (+%.0f%%)", (unsigned long long)base, (unsigned long long)m_pixelBytes,
                100.0 * (double)(m_pixelBytes - base) / (double)base);
    ImGui::Text("Upload 버퍼: %llu B (빈칸 포함)", (unsigned long long)m_uploadBytes);
}

void TextureScene::PanelEli5()
{
    ImGui::SeparatorText("어려운 말 풀이");
    Eli5("텍스처 (Texture)",
         "벽에 붙이는 벽지. 벽(삼각형)은 모양만 있고, 무늬는 벽지가 가져요.",
         "GPU 메모리에 있는 2차원 픽셀 배열이에요. 픽셀 하나를 \"텍셀\"(텍스처의 픽셀)이라고 불러요.",
         "셰이더가 위치(uv)를 주면 그 자리의 색을 돌려줘요.",
         "꼭짓점마다 색을 주는 것만으로는 사진 같은 무늬를 못 만들어요. 꼭짓점은 3개뿐이지만 텍셀은 6만 5천 개니까요.",
         "이 장면: 256x256 RGBA8 = 텍셀 65,536개 x 4바이트 = 256KB (밉 0 만).");
    Eli5("UV 좌표",
         "지도의 위도·경도. 지도가 크든 작든 \"가로 30%, 세로 70% 지점\"은 같은 곳이에요.",
         "그림 안의 위치를 0..1 로 나타낸 값. u = 가로(왼쪽 0 -> 오른쪽 1), v = 세로(위 0 -> 아래 1).",
         "셰이더가 uv 를 정하면 샘플러가 그 자리의 텍셀을 찾아요. 텍셀 번호 = uv x 그림 크기.",
         "그림 크기(256, 1024 ...)가 바뀌어도 셰이더 코드를 안 고쳐도 되게. 그리고 밉마다 크기가 다른데 같은 uv 로 읽을 수 있게.",
         "uv (0.5, 0.5) = 256x256 에선 텍셀 (128, 128), 밉 3 (32x32) 에선 텍셀 (16, 16). 같은 \"가운데\"예요.");
    Eli5("SRV (셰이더 리소스 뷰) 카드",
         "도서관 대출 카드. 책(텍스처) 자체가 아니라 \"몇 층 몇 번 서가, 이런 책\"이 적힌 카드.",
         "디스크립터의 한 종류. \"이 메모리를 RGBA8 2D 그림으로, 밉 0..8 을 보면서 읽어라\"가 적혀 있어요.",
         "주소록(SRV 힙)에 꽂아 두면, 셰이더의 t0 이 그 카드를 통해 텍스처를 읽어요.",
         "메모리만 보고는 그게 그림인지, 몇 픽셀인지, 형식이 뭔지 GPU 가 몰라요. 같은 메모리도 카드에 따라 다르게 읽을 수 있어요.",
         "이 장면: 텍스처 2장에 카드 2장. \"밉 레벨 색칠\" 체크는 칸 0 에 넣는 카드 위치만 바꿔요 (그림 데이터는 안 건드림).");
    Eli5("샘플러와 필터",
         "돋보기 사용법. 그림은 같아도 \"가장 가까운 점 하나만 본다\"와 \"주변을 섞어 본다\"는 결과가 달라요.",
         "샘플러 = 텍스처를 읽는 규칙 묶음 (필터, 주소 모드, 밉 범위). 필터 = 텍셀 사이를 어떻게 메울지.",
         "POINT 는 가장 가까운 텍셀 하나, LINEAR 는 주변 4개를 거리만큼 섞기, ANISOTROPIC 은 비스듬한 방향으로 여러 번 더 읽기.",
         "화면 픽셀과 텍셀이 1:1 로 딱 맞는 경우는 거의 없어요. 확대·축소·기울어짐마다 \"사이 값\"을 정해야 해요.",
         "평면을 8배 확대: POINT = 계단진 동그라미, LINEAR = 부드럽지만 흐린 동그라미.");
    Eli5("주소 모드 (Address mode)",
         "벽지가 벽보다 작을 때: 같은 벽지를 이어 붙이기(WRAP), 마지막 줄을 쭉 늘이기(CLAMP), 뒤집어 붙이기(MIRROR).",
         "uv 가 0..1 밖일 때 어느 텍셀을 읽을지 정하는 규칙.",
         "WRAP: 소수 부분만 (1.3 -> 0.3). CLAMP: 0..1 로 자르기 (1.3 -> 1). MIRROR: 1을 넘으면 거꾸로 (1.3 -> 0.7).",
         "작은 타일 그림 하나로 넓은 바닥을 덮으려면 반복이 필요하고, 반복하면 안 되는 그림도 있어서.",
         "UV 배수 2 + 오른쪽 아래 끝: WRAP = 파란 칸, MIRROR = 빨간 칸(뒤집혀서 왼쪽 위), CLAMP = 파랑이 쭉 늘어남.");
    Eli5("밉맵 (Mipmap)",
         "같은 사진을 원본, 1/2, 1/4 ... 크기로 미리 인화해 둔 앨범. 멀리 걸 액자엔 작은 사진을 쓰면 돼요.",
         "원래 그림을 반씩 줄인 그림들 (256, 128, ... 1). 레벨 0 이 원본. MIP = 라틴어 \"좁은 곳에 많이\"의 줄임말.",
         "GPU 가 픽셀마다 \"이 픽셀이 텍셀 몇 개를 덮나\"를 보고 알맞은 레벨을 골라 읽어요 (Sample 함수가 자동으로).",
         "먼 곳의 픽셀 하나가 텍셀 수십 개를 덮을 때, 그중 하나만 골라 읽으면 움직일 때마다 색이 튀어요(지글거림). 미리 평균 낸 그림을 읽으면 차분해요. "
         "덤으로 작은 그림을 읽으니 캐시에도 잘 맞아서 빨라요.",
         "메모리는 +33% (1/4 + 1/16 + ... = 1/3). 이 장면 밉 표에서 직접 확인해요. DX12 에는 GenerateMips 가 없어서 이 장면은 CPU 에서 2x2 평균으로 만들어요.");
    Eli5("Default 힙과 복사",
         "창고(VRAM) 안은 직원(GPU)만 들어가요. 손님(CPU)은 택배 상자(Upload 버퍼)에 담아 문 앞에 두고, 직원이 안으로 옮겨요.",
         "Default 힙 = VRAM, GPU 전용. Upload 힙 = RAM, CPU 가 쓰고 GPU 가 읽을 수 있음. CopyTextureRegion = GPU 에게 시키는 복사.",
         "CPU 가 Upload 버퍼에 그림을 쓰고 -> 명령 목록에 복사 명령을 넣고 -> GPU 가 실행하면 VRAM 에 그림이 생겨요.",
         "텍스처는 매 프레임 많이 읽어서 VRAM 에 있어야 빨라요. 그런데 CPU 는 VRAM 에 직접 못 써요 (발표의 BAR/ReBAR 이야기).",
         "이 장면: Upload 버퍼 약 350KB x 2 -> 복사 -> 다음에 같은 프레임 세트가 돌아오면(GPU 끝남) Upload 버퍼 지우기.");
    Eli5("배리어 (Resource barrier)",
         "공사 중 표지판. \"지금은 짐 넣는 중(COPY_DEST)\" 표지를 \"이제 구경해도 됨(PIXEL_SHADER_RESOURCE)\"으로 바꿔 거는 것.",
         "리소스의 \"지금 용도(상태)\"를 바꾼다고 GPU 에게 알리는 명령.",
         "GPU 가 앞의 작업(복사)을 다 끝낸 뒤에 다음 작업(읽기)을 하게 하고, 필요하면 메모리 배치나 캐시도 정리해요.",
         "GPU 는 여러 일을 동시에 해요. 알려 주지 않으면 복사가 반쯤 됐을 때 읽어 버릴 수 있어요.",
         "이 장면: 복사 9번 바로 뒤에 배리어 1번 (텍스처 2장이라 한꺼번에 2개).");
    Eli5("샘플러 힙",
         "SRV 카드 주소록과 따로 있는 \"읽는 규칙 카드\" 주소록.",
         "샘플러 디스크립터만 담는 디스크립터 힙 (최대 2048장).",
         "루트 시그니처의 샘플러 테이블(칸 1)이 이 주소록의 위치를 가리키고, 셰이더의 s0 이 그 카드를 써요.",
         "샘플러 카드는 종류가 적고 많이 재사용돼서, GPU 가 따로 관리하게 나눠 놨어요. 그래서 SetDescriptorHeaps 에 주소록 2권을 같이 펼쳐요.",
         "이 장면: 18장 만들어 두고, 버튼을 누르면 카드 번호만 바꿈. (자주 안 바뀌는 샘플러는 루트 시그니처에 \"정적 샘플러\"로 박아 둘 수도 있어요)");
    Eli5("RowPitch 256바이트 정렬",
         "공책 줄. 글자가 짧아도 다음 줄은 항상 정해진 칸에서 시작해요.",
         "복사용 버퍼에서 그림 한 줄과 다음 줄 사이의 바이트 간격. D3D12 는 256의 배수로 요구해요.",
         "CPU 가 Upload 버퍼에 쓸 때 줄마다 RowPitch 만큼 건너뛰어 써요. 너비 x 4 로 건너뛰면 그림이 비스듬히 밀려요.",
         "GPU 복사 장치가 정해진 간격으로 읽어야 빨라서.",
         "32x32 밉: 한 줄 128바이트 -> 간격 256바이트 (128바이트는 빈칸). 1x1 밉도 256바이트 한 줄.");
}

void TextureScene::PanelCodeFlow()
{
    if (!ImGui::CollapsingHeader("이 장면의 DX12 코드 흐름")) return;
    ImGui::Indent();
    ImGui::TextWrapped("[텍스처-①] MakeImages(): CPU 배열에 체크무늬 그리기 + 2x2 평균으로 밉 8장 + 밉 색칠 그림.");
    ImGui::TextWrapped("[텍스처-②] CreateTextures(): D3D12_RESOURCE_DESC(256x256, 밉 9) 로 Default 힙 텍스처, 처음 상태 COPY_DEST.");
    ImGui::TextWrapped("[텍스처-③] GetCopyableFootprints() 로 밉마다 시작 위치, 줄 간격을 받고 WriteUpload() 가 줄마다 memcpy.");
    ImGui::TextWrapped("[텍스처-④] RecordCopies(): 첫 프레임에 CopyTextureRegion x 9 + 배리어. OnFrameReused() 에서 Upload 버퍼 지우기.");
    ImGui::TextWrapped("[텍스처-⑤] CreateSrvs(): SRV 카드 (형식, 2D, 밉 9장).");
    ImGui::TextWrapped("[텍스처-⑥] CreateSamplers(): 샘플러 힙 18장 (필터 x 주소 x 밉).");
    ImGui::TextWrapped("[텍스처-⑦] CreateRootSig() + OnRender(): SetDescriptorHeaps(2권) -> 테이블 2개 + 루트 상수 -> 그리기.");
    ImGui::TextWrapped("[텍스처-⑧] kQuad + CreateMesh: 평면 보기 네모 = 정점 4개(위치 + uv, 16바이트) + 인덱스 6개를 VRAM 에. 그리기는 DrawIndexedInstanced(6). 바닥 보기는 화면을 덮는 삼각형 DrawInstanced(3).");
    ImGui::Unindent();
}

// ---------------------------------------------------------------------
// 개발용 자동 테스트 (LAB_AUTOTEST=1, LAB_SCENE=3)
//  화면의 정해진 점 5개를 되읽어서 색을 본다.
//   점 0: 평면 네모의 왼쪽 위 칸 가운데      점 1: 오른쪽 위 칸 가운데
//   점 2: 네모 안 (0.97, 0.97) 지점         점 3: 네모 안 (0.6, 0.6) 지점
//   점 4: 바닥 보기의 먼 곳
// ---------------------------------------------------------------------
void TextureScene::ProbePixel(UINT i, const Canvas& c, UINT& px, UINT& py) const
{
    const float bxs[4] = { 1.0f / 16, 15.0f / 16, 0.97f, 0.6f };   // 네모 안 위치 (0..1)
    const float bys[4] = { 1.0f / 16, 1.0f / 16, 0.97f, 0.6f };
    float x = 0, y = 0;   // NDC
    if (i < 4)
    {
        const float bx = bxs[i], by = bys[i];
        const float half = 0.8f / m_zoom;
        const float qx = (bx - 0.5f) * 2.0f * half, qy = (by - 0.5f) * 2.0f * half;
        x = qx / m_aspect; y = -qy;
    }
    else { x = 0.13f; y = 0.25f; }
    px = (UINT)std::clamp((x * 0.5f + 0.5f) * c.w, 0.0f, c.w - 1);
    py = (UINT)std::clamp((-y * 0.5f + 0.5f) * c.h, 0.0f, c.h - 1);
}

void TextureScene::CaptureProbes(ID3D12GraphicsCommandList* cl, const Canvas& c)
{
    const UINT f = g_dx.frameIndex;
    m_capTag[f] = m_testCase;
    ID3D12Resource* back = g_dx.backBuffers[g_dx.swapChain->GetCurrentBackBufferIndex()].Get();
    D3D12_RESOURCE_BARRIER b = Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &b);
    for (UINT i = 0; i < kProbes; i++)
    {
        UINT px, py;
        ProbePixel(i, c, px, py);
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

// 색 이름: 0 빨강, 1 초록, 2 노랑, 3 파랑, -1 그 밖
static int ColorClass(const unsigned* c)
{
    const bool r = c[0] > 150, g = c[1] > 150, b = c[2] > 150;
    const bool lr = c[0] < 110, lg = c[1] < 110, lb = c[2] < 110;
    if (r && lg && lb) return 0;
    if (g && lr && lb) return 1;
    if (r && g && lb) return 2;
    if (b && lr && lg) return 3;
    return -1;
}

static const int kTestCases = 6;
// 케이스: 0 평면 기본(모서리 색), 1 UV2 WRAP, 2 UV2 MIRROR, 3 UV2 CLAMP, 4 바닥 밉 켬(색칠), 5 바닥 밉 끔(색칠)
void TextureScene::ApplyTestCase(int k)
{
    m_view = k >= 4 ? 1 : 0;
    m_filter = k >= 4 ? kLinear : kPoint;
    m_address = (k == 2) ? kMirror : (k == 3) ? kClamp : kWrap;
    m_uvScale = (k >= 1 && k <= 3) ? 2.0f : 1.0f;
    m_zoom = 1.0f;
    m_mips = k != 5;
    m_tinted = k >= 4;
    m_move = false;
    m_scroll = 0.0f;
    m_testWait = 0;
}

bool TextureScene::AutoTest(int frame, int& failures)
{
    if (frame < 3) return false;
    if (m_testCase < 0) { m_testCase = 0; ApplyTestCase(0); return false; }
    bool ok = true;
    const char* what = "";
    if (++m_testWait > 60) { ok = false; what = "TIMEOUT"; }
    else if (m_gotTag != m_testCase || m_testWait < 4) return false;
    else
    {
        switch (m_testCase)
        {
        case 0: ok = ColorClass(m_gotRgb[0]) == 0 && ColorClass(m_gotRgb[1]) == 1; what = "TL red, TR green"; break;
        case 1: ok = ColorClass(m_gotRgb[2]) == 3; what = "WRAP (0.97)x2 -> blue"; break;
        case 2: ok = ColorClass(m_gotRgb[2]) == 0; what = "MIRROR (0.97)x2 -> red"; break;
        case 3: ok = ColorClass(m_gotRgb[3]) == 3; what = "CLAMP (0.6)x2 -> blue"; break;
        case 4: for (int j = 0; j < 3; j++) m_floorRgb[0][j] = m_gotRgb[4][j]; what = "floor mips on (record)"; break;
        case 5:
        {
            for (int j = 0; j < 3; j++) m_floorRgb[1][j] = m_gotRgb[4][j];
            int d = 0;
            for (int j = 0; j < 3; j++) d += std::abs((int)m_floorRgb[0][j] - (int)m_floorRgb[1][j]);
            ok = d > 60; what = "floor mips on vs off differ";
            break;
        }
        }
    }
    if (!ok) failures++;
    FILE* f = fopen("lab_autotest.log", "a");
    if (f)
    {
        fprintf(f, "tex case %d %s:", m_testCase, what);
        for (UINT i = 0; i < kProbes; i++) fprintf(f, " [%u]=(%u,%u,%u)", i, m_gotRgb[i][0], m_gotRgb[i][1], m_gotRgb[i][2]);
        fprintf(f, " upload=%d %s\n", m_uploadState, ok ? "ok" : "FAIL");
        fclose(f);
    }
    if (++m_testCase >= kTestCases)
    {
        if (m_uploadState != kDone) { failures++; f = fopen("lab_autotest.log", "a"); if (f) { fprintf(f, "tex upload not freed FAIL\n"); fclose(f); } }
        return true;
    }
    ApplyTestCase(m_testCase);
    m_gotTag = -1;
    return false;
}

} // namespace

Scene* CreateTextureScene() { return new TextureScene(); }
