// =====================================================================
//  playground.hlsl  -  장면 2 (픽셀 셰이더 놀이터) 의 셰이더
//
//  Shadertoy 와 같은 구조예요: 화면(왼쪽 그림 영역)을 덮는 삼각형 하나 + 픽셀마다 mainImage 한 번.
//  여러분이 고칠 곳은 맨 아래 mainImage 하나뿐이에요. 저장(Ctrl+S)하면 바로 반영돼요.
//  망가뜨려도 괜찮아요. 원래대로 돌리려면:  git checkout lab/shaders/playground.hlsl
//
//  흐름:
//    FullVS -> [래스터라이저: 그림 영역의 모든 픽셀을 고름] -> MainPS -> mainImage(fragColor, fragCoord)
//  함수 이름 2개(FullVS, MainPS)는 C++ 이 찾으니까 바꾸지 마세요.
//
//  해 볼 실험 (학습 로드맵 1단계)
//    ① 원을 가운데에 멈추기:  float2 c = float2(0.3 * sin(iTime), 0.0);  ->  float2 c = float2(0.0, 0.0);
//    ② 시간으로 색 바꾸기:   bg 의 파랑 자리 0.5 + 0.5 * sin(iTime) 를 빨강 자리로 옮겨 보기
//    ③ 마우스로 원 옮기기:   c = (iMouse.xy - 0.5 * iResolution.xy) / iResolution.y;
//                            (그림 영역을 누른 채 끌면 iMouse.xy 가 바뀌어요)
//    ④ Shadertoy 예제 하나 옮기기: 예제의 mainImage 를 통째로 붙여 넣고 아래 "GLSL -> HLSL" 표대로 고치기
//
//  GLSL(Shadertoy) -> HLSL 바꾸는 표 (아래 #define 덕분에 vec2, mix 같은 건 그대로 둬도 돼요)
//    vec2 / vec3 / vec4      -> float2 / float3 / float4   (#define 있음)
//    mix / fract / mod       -> lerp / frac / Mod          (#define 있음. GLSL mod 는 음수에서 fmod 와 달라서 Mod 함수를 따로 둠)
//    vec3(1.0)  (숫자 하나)  -> (float3)1.0 또는 float3(1.0, 1.0, 1.0)   <- 자동으로 안 돼요, 직접 고치기
//    atan(y, x)              -> atan2(y, x)                <- 직접 고치기
//    m * v  (행렬 곱)        -> mul(v, m)                  <- 직접 고치기 (순서 주의)
//    const float x = 1.0;    -> static const float x = 1.0;  (함수 밖일 때)
//    texture(iChannel0, uv)  -> 이 놀이터에는 텍스처가 없어요 (로드맵 3단계에서 배워요)
//
//  ④ 연습용: Shadertoy 에서 "New" 를 누르면 나오는 기본 예제예요. 아래 mainImage 를 통째로 이걸로 바꿔 보세요.
//    (GLSL 그대로인데 위 #define 덕분에 고칠 곳이 없어요. 무지개색이 흘러가면 성공)
//      void mainImage(out vec4 fragColor, in vec2 fragCoord)
//      {
//          vec2 uv = fragCoord / iResolution.xy;
//          vec3 col = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4));
//          fragColor = vec4(col, 1.0);
//      }
//
//  도전 과제 답안 (로드맵 1단계 "바다 위 해 지는 풍경", Ming 이 직접 짠 코드 2026-11-13)
//    하늘 그라데이션 + 칼같은 수평선 + 10초마다 지는 해 + 점점 어두워지는 하늘.
//    써 보려면 아래 mainImage 를 통째로 이걸로 바꾸고 맨 앞의 "//" 를 지우세요.
//      void mainImage(out vec4 fragColor, in vec2 fragCoord)
//      {
//          float3 blue = float3(0.0, 0.0, 1.0);
//          float3 orange = float3(1.0, 0.55, 0.1);
//          float3 red = float3(1.0, 0.0, 0.0);
//          float2 uv = fragCoord / iResolution.xy;
//
//          // 배경 그라데이션
//          float3 g = float3(0.0, 0.0, 0.0);
//          float f = fract(iTime * 0.1);   // 0 -> 1
//          if(step(0.3, uv.y))
//          {
//              float t = (uv.y - 0.3) / 0.7;
//              g = lerp(orange, blue, t);
//
//              // 움직이는 해(잘린 부분부터는 바다 뒤로 가야하기 때문에 여기까지 그린다.)
//              float2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
//
//              float r = 0.1;  // 반지름
//              float y = lerp(0.5, -0.2 - r, f);   // 화면 맨 위 -> 수평선 아래로 완전히
//              float2 c = float2(0.0, y); // 화면 맨 위(0.5) -> 수평선 아래(-0.2 - r)
//
//              float d = length(p - c);
//              float inside = step(d, r);
//              if(inside)
//                  g = red;
//          }
//          else
//              g = blue;
//
//          // 점점 어두워지게 하기
//          float time = lerp(1.0, 0.3, f); //너무 어두워지지 않게
//          g = g * time;
//          fragColor = float4(g, 1.0);
//      }
// =====================================================================

// 루트 상수 8개 (float 8개 = 32바이트). C++ 의 PlayConstants 와 순서가 같아야 해요.
// "uniform" = 모든 픽셀이 똑같이 받는 값. 칠판에 적어 둔 숫자를 학생 전원이 보는 것과 같아요.
cbuffer PlayConstants : register(b0)
{
    float2 gResolution;   // 그림 영역 크기 (픽셀)
    float  gTime;         // 시작부터 흐른 시간 (초)
    float  gFrame;        // 그린 프레임 수
    float4 gMouse;        // xy = 마우스 위치(픽셀, 왼쪽 아래가 0), zw = 누른 위치 (안 누르면 음수)
};

// ---- Shadertoy 이름 그대로 쓰기 ----
#define iResolution float3(gResolution, 1.0)
#define iTime       gTime
#define iFrame      ((int)gFrame)
#define iMouse      gMouse
#define vec2  float2
#define vec3  float3
#define vec4  float4
#define mat2  float2x2
#define mat3  float3x3
#define mix   lerp
#define fract frac
float  Mod(float  x, float  y) { return x - y * floor(x / y); }   // GLSL 의 mod 와 같은 결과
float2 Mod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 Mod(float3 x, float3 y) { return x - y * floor(x / y); }
#define mod Mod

// ---------------------------------------------------------------------
// 여기만 고치면 돼요. fragCoord = 이 픽셀의 위치 (왼쪽 아래가 0, Shadertoy 와 같음)
// ---------------------------------------------------------------------
void mainImage(out float4 fragColor, in float2 fragCoord)
{
    float2 uv = fragCoord / iResolution.xy;                          // 0..1 (왼쪽 아래 0, 오른쪽 위 1)
    float2 p  = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;  // 가운데가 0, 위아래가 -0.5..0.5 (원이 찌그러지지 않게)

    float2 c = float2(0.3 * sin(iTime), 0.0);   // 원의 중심: 시간에 따라 좌우로
    float  d = length(p - c);                   // 이 픽셀에서 중심까지 거리 (SDF)
    float  r = 0.2, w = 0.01;                   // 반지름, 테두리 부드러움
    float  inside = 1.0 - smoothstep(r, r + w, d);   // 안 = 1, 밖 = 0, 경계는 부드럽게

    float3 bg = float3(uv, 0.5 + 0.5 * sin(iTime));  // 배경: x 는 빨강, y 는 초록, 시간은 파랑
    fragColor = float4(lerp(bg, float3(1.0, 1.0, 1.0), inside), 1.0);
}

// ---------------------------------------------------------------------
// 아래는 Shadertoy 가 우리 몰래 해 주던 부분이에요 (보통은 안 고쳐도 돼요)
// ---------------------------------------------------------------------

// 화면을 덮는 큰 삼각형 하나: 정점 버퍼 없이 SV_VertexID(0, 1, 2)로 꼭짓점을 만든다
float4 FullVS(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((float)((id << 1) & 2), (float)(id & 2));   // (0,0) (2,0) (0,2)
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);    // 화면보다 큰 삼각형 (밖은 잘려 나감)
}

// 픽셀마다 한 번. SV_Position 은 위쪽이 0 이라서, Shadertoy 처럼 아래쪽이 0 이 되게 뒤집어서 넘긴다
float4 MainPS(float4 pos : SV_Position) : SV_Target
{
    float2 fragCoord = float2(pos.x, gResolution.y - pos.y);
    float4 col = float4(0, 0, 0, 1);
    mainImage(col, fragCoord);
    return float4(col.rgb, 1.0);
}
