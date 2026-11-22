// =====================================================================
//  tex.hlsl  -  장면 4 (텍스처 붙이기: 그림 한 장을 VRAM 에 올려서 읽기) 의 셰이더
//
//  새로 나오는 것 두 가지:
//    Texture2D gTex  : register(t0)  -> 그림 (C++ 이 VRAM 에 올려 둔 256x256 체크무늬, 밉 9장)
//    SamplerState    : register(s0)  -> 그림을 "어떻게" 읽을지 정한 규칙 (필터, 주소 모드, 밉)
//  읽는 줄은 딱 한 줄:  gTex.Sample(gSamp, uv)
//  uv 는 "그림 안의 위치"예요. (0, 0) = 그림 왼쪽 위, (1, 1) = 오른쪽 아래. 픽셀 개수와 상관없어요.
//
//  보기 방법은 두 가지이고, 그리는 방법도 달라요 (둘 다 실제 게임에서 쓰는 방법):
//    평면 : 네모(점 4개 + 인덱스 6개)를 정점 버퍼로 그려요. 점마다 uv 가 정점 버퍼에 들어 있어요 (TEXCOORD).
//           게임에서 그림 붙은 판, UI, 모델 표면이 다 이렇게 와요. uv 를 1보다 크게 하면(패널의 "UV 배수") 주소 모드가 보여요.
//    바닥 : "화면을 덮는 삼각형 하나"(정점 버퍼 없음, SV_VertexID)로 그리고, 픽셀 셰이더가 픽셀마다 uv 를 계산해요.
//           게임의 화면 전체 효과(후처리, 하늘)도 이렇게 그려요. 멀수록 그림이 작아져서 밉맵이 필요해져요.
//
//  함수 이름 3개(TexQuadVS, TexVS, TexPS)는 C++ 이 찾으니까 바꾸지 마세요.
//  망가뜨려도 괜찮아요. 셰이더 창의 "처음 코드로" 버튼으로 돌아가요.
//
//  해 볼 실험 (셰이더 쪽. 패널 쪽 실험 3개는 로드맵 3단계에 있어요)
//    ① 그림 뒤집기 (UV 배수 1, 평면 보기에서):
//       TexPS 의  if (gView > 0.5) uv = FloorUv(i.p);  다음 줄에  uv.y = 1.0 - uv.y;  추가
//       -> 빨강 칸(왼쪽 위)이 왼쪽 아래로 가요. 이미지 파일마다 위아래 기준이 달라서 실제로 자주 만나는 버그예요.
//    ② 밉을 내가 고르기:  float4 c = gTex.Sample(gSamp, uv);  ->  float4 c = gTex.SampleLevel(gSamp, uv, 3);
//       -> 늘 밉 3번(32x32)만 읽어서 흐릿해져요. "밉 레벨 색칠"을 켜면 전부 노란색이에요.
//          Sample 은 GPU 가 "이 픽셀이 그림을 얼마나 작게 보나"를 보고 밉을 알아서 골라요. SampleLevel 은 직접 정해요.
//    ③ 색 채널 바꾸기:  평면 쪽  return float4(c.rgb * light, 1.0);  ->  return float4(c.bgr * light, 1.0);
//       -> 빨강과 파랑이 바뀌어요. 텍스처는 결국 숫자 4개(R, G, B, A)의 배열이라는 걸 보여 줘요.
//    (실험이 끝나면 되돌리거나 "처음 코드로")
// =====================================================================

Texture2D    gTex  : register(t0);   // 그림. C++ 의 SRV 카드(디스크립터)가 가리키는 VRAM 텍스처
SamplerState gSamp : register(s0);   // 읽는 규칙. C++ 의 샘플러 카드 (패널에서 바꾸는 것)

// C++ 의 TexConstants 와 순서가 같아야 해요 (float 8개 = 32바이트, 루트 상수)
cbuffer TexConstants : register(b0)
{
    float gAspect;    // 그림 영역 너비 / 높이 (네모가 찌그러지지 않게)
    float gUvScale;   // UV 배수: 1 = 그림 한 장, 3 = uv 가 0..3 까지 (그림 3x3 장 자리)
    float gView;      // 0 = 평면, 1 = 바닥
    float gScroll;    // 바닥이 앞으로 움직인 거리
    float gZoom;      // 평면 보기 확대 (1 = 기본, 8 = 8배 확대)
    float gPad0, gPad1, gPad2;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 p   : SCREENPOS;   // 화면 위치 (-1..1, 위가 +1)
    float2 uv  : TEXCOORD;    // 평면 보기: 정점 버퍼의 uv 를 픽셀마다 섞은 값
};

// [텍스처-⑧] 평면 보기의 네모 정점 (C++ 의 QuadVertex 와 입력 레이아웃과 짝, 16바이트)
struct QuadVertex
{
    float2 position : POSITION;   // 네모 모서리 (-1..1, 위가 +1)
    float2 uv       : TEXCOORD;   // 그림 안의 위치 (왼쪽 위 0, 0 / 오른쪽 아래 1, 1)
};

// 평면 보기: 정점 버퍼에서 온 네모를 화면 가운데에 놓고, uv 에 배수를 곱해요
VSOut TexQuadVS(QuadVertex v)
{
    float halfSize = 0.8 / gZoom;                          // 네모 반지름 (화면 높이의 0.8 배)
    float2 p = float2(v.position.x * halfSize / gAspect,   // 가로세로 비율 맞추기 (찌그러지지 않게)
                      v.position.y * halfSize);
    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.p   = p;
    o.uv  = v.uv * gUvScale;                               // 배수 2 면 uv 가 0..2 -> 주소 모드가 보여요
    return o;
}

// 바닥 보기: 화면을 덮는 큰 삼각형 하나 (장면 2와 같은 방법, 정점 버퍼 없음)
VSOut TexVS(uint id : SV_VertexID)
{
    float2 p = float2(-1.0, -1.0);
    if (id == 1) p = float2(-1.0, 3.0);
    if (id == 2) p = float2( 3.0, -1.0);
    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.p = p;
    o.uv = float2(0.0, 0.0);   // 바닥은 픽셀 셰이더가 FloorUv 로 계산해요
    return o;
}

// 바닥 보기: 수평선(p.y = 0.35) 아래가 바닥. 수평선에 가까울수록 멀리 있는 바닥
float2 FloorUv(float2 p)
{
    float depth = 1.0 / max(0.35 - p.y, 0.003);   // 거리. 화면 아래 = 가까움(작음), 수평선 = 아주 멂(큼)
    return float2(p.x * gAspect * depth, depth + gScroll) * 0.5 * gUvScale;
}

float4 TexPS(VSOut i) : SV_Target
{
    float2 uv = i.uv;                     // 평면: 정점 버퍼의 uv (래스터라이저가 픽셀마다 섞어 줌)
    if (gView > 0.5) uv = FloorUv(i.p);   // gView 는 모든 픽셀이 같은 값이라 이런 if 는 괜찮아요

    // ★ 그림 읽기. 이 한 줄이 이 장면의 주인공이에요
    float4 c = gTex.Sample(gSamp, uv);

    float light = 1.0;
    if (gView < 0.5)
    {
        // 평면: 네모 안만 그려지니까 그대로 (네모 밖은 아예 픽셀 셰이더가 안 돌아요)
        return float4(c.rgb * light, 1.0);
    }

    // 바닥: 멀수록 살짝 어둡게, 수평선 위는 하늘
    float far = saturate((i.p.y + 1.0) / 1.35);       // 화면 아래 0 -> 수평선 1
    light = lerp(1.0, 0.75, far);
    float3 sky = lerp(float3(0.55, 0.75, 0.95), float3(0.2, 0.35, 0.7), saturate((i.p.y - 0.35) / 0.65));
    float isSky = step(0.35, i.p.y);
    return float4(lerp(c.rgb * light, sky, isSky), 1.0);
}
