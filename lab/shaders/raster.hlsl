// =====================================================================
//  raster.hlsl  -  장면 1 (래스터라이저) 의 셰이더
//
//  이 파일을 고치고 저장하면 앱이 알아서 다시 컴파일해서 바로 화면에 반영해요.
//  (앱 안 "셰이더 편집 창"에서 고쳐도 되고, VS / VS Code / 메모장에서 고쳐도 돼요.)
//  틀리면 앱은 꺼지지 않고, 편집 창에 에러와 줄 번호가 빨간 글씨로 나와요.
//  망가뜨려도 괜찮아요. 원래대로 돌리려면:  git checkout lab/shaders/raster.hlsl
//
//  흐름 (HelloTriangle 의 shaders.hlsl 과 같은 구조):
//    1패스  TriVS  -> [래스터라이저: 칸 고르기 + wa,wb,wc 로 섞기] -> TriPS   : 작은 그림(칸 수 x 칸 수 픽셀)에 삼각형
//    2패스  BlitVS -> [래스터라이저] -> BlitPS                                 : 작은 그림을 화면에 크게 늘리기
//  함수 이름 4개(TriVS, TriPS, BlitVS, BlitPS)는 C++ 이 찾으니까 바꾸지 마세요.
//
//  해 볼 실험 (GPU 결과 보기에서 확인, 비교 보기에서는 CPU 와 달라진 칸이 분홍으로 나와요)
//    ① TriPS: return float4(i.col.rgb, 1.0);  ->  return float4(i.col.r, 0.0, 0.0, 1.0);
//       빨강(wa)만 남겨요. A 에서 멀어질수록 어두워지는 게 wa 그 자체예요.
//    ② TriPS: return float4(1.0 - i.col.rgb, 1.0);   색 반전
//    ③ TriVS: o.col = c;  ->  o.col = float4(1, 1, 1, 1);   모든 꼭짓점이 흰색이면 섞어도 흰색
//    ④ TriPS: if (i.col.r < 0.5) discard;   wa 가 0.5 보다 작은 칸은 안 칠해요 (discard = 이 픽셀은 버려)
//    ⑤ TriPS: SV_Position 을 받아서 칸 좌표로 체크무늬 만들기 (아래 주석 참고)
// =====================================================================

// 루트 상수 20개 (float 20개 = 80바이트). C++ 의 TriConstants 와 순서가 같아야 해요.
cbuffer TriConstants : register(b0)
{
    float2 gA;    float2 gB;          // 꼭짓점 위치 (단위: 칸)
    float2 gC;    float2 gGrid;       // gGrid = 작은 렌더 타깃 크기 (가로 칸 수, 세로 칸 수)
    float4 gColA; float4 gColB; float4 gColC;   // 꼭짓점 색 (A 빨강, B 초록, C 파랑)
};
Texture2D    gLow   : register(t0);   // 1패스 결과 (작은 그림)
SamplerState gPoint : register(s0);   // POINT: 가장 가까운 픽셀 하나만 읽음 (흐려지지 않음)

struct VSOut
{
    float4 pos : SV_Position;   // SV_ = 시스템이 쓰는 값. 래스터라이저가 이 위치로 칸을 골라요
    float4 col : COLOR0;        // SV_ 가 아닌 값 = 래스터라이저가 wa, wb, wc 로 섞어서 PS 에 넘겨 줘요
};

// [래스터-④] 1패스 VS: 정점 버퍼가 없어요. SV_VertexID(0, 1, 2)로 어느 꼭짓점인지 골라요
VSOut TriVS(uint id : SV_VertexID)
{
    float2 p = gA; float4 c = gColA;
    if (id == 1) { p = gB; c = gColB; }
    if (id == 2) { p = gC; c = gColC; }

    VSOut o;
    // 칸 좌표 -> NDC(-1..1).  x: 0..가로 -> -1..1,  y: 0..세로 -> 1..-1 (화면은 아래로, NDC 는 위로)
    o.pos = float4(p.x / gGrid.x * 2.0 - 1.0, 1.0 - p.y / gGrid.y * 2.0, 0.0, 1.0);
    o.col = c;
    return o;
}

// 1패스 PS: 래스터라이저가 wa*A색 + wb*B색 + wc*C색 으로 섞어 준 색이 i.col 로 들어와요.
// A=빨강, B=초록, C=파랑이라 i.col.rgb 가 그대로 (wa, wb, wc) 예요.
float4 TriPS(VSOut i) : SV_Target
{
    // ⑤ 체크무늬 실험: i.pos.xy 는 이 픽셀의 중심 좌표예요 (예: 3.5, 2.5)
    // float2 cell = floor(i.pos.xy);
    // if (fmod(cell.x + cell.y, 2.0) == 0.0) return float4(i.col.rgb * 0.5, 1.0);
    return float4(i.col.rgb, 1.0);
}

// [래스터-⑤] 2패스: 화면을 덮는 큰 삼각형 하나로 작은 그림을 늘려 그려요
struct BlitOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

BlitOut BlitVS(uint id : SV_VertexID)
{
    BlitOut o;
    float2 uv = float2((float)((id << 1) & 2), (float)(id & 2));   // (0,0) (2,0) (0,2)
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

float4 BlitPS(BlitOut i) : SV_Target
{
    float4 t = gLow.Sample(gPoint, i.uv);
    if (t.a < 0.5) return float4(0.10, 0.10, 0.13, 1.0);   // 안 칠한 칸 = 배경색
    return float4(t.rgb, 1.0);
}
