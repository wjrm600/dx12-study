// =====================================================================
//  spin.hlsl  -  장면 4 (돌아가는 삼각형: 상수를 GPU 에 보내는 세 가지 길) 의 셰이더
//
//  이 장면의 주인공은 셰이더가 아니라 "값이 어떤 길로 오나"예요.
//  그래서 이 셰이더는 일부러 아주 짧아요. 그리고 방법 A, B, C 가 모두 이 파일 하나를 같이 써요.
//  셰이더는 "b0 에 SpinConstants 가 있다"만 알고, 그 값이 어떤 길로 왔는지는 몰라요.
//  길은 C++ 의 루트 시그니처(준비물 가방)가 정해요.
//
//  삼각형 모양(꼭짓점 3개)은 정점 버퍼에서 와요 (게임 방식, 장면 2 와 같은 mesh.h 도우미).
//  비교용으로 정점 버퍼 없이 번호(SV_VertexID)로 만드는 SpinVSNoBuffer 도 남겨 뒀어요 (패널의 체크 상자).
//
//  함수 이름 3개(SpinVS, SpinVSNoBuffer, SpinPS)는 C++ 이 찾으니까 바꾸지 마세요.
//  망가뜨려도 괜찮아요. 셰이더 창의 "처음 코드로" 버튼으로 돌아가요.
//
//  해 볼 실험
//    ① SpinPS 에서  return float4(gColor.rgb * i.shade, 1.0);  ->  return float4(gColor.rgb, 1.0);
//       꼭짓점마다 다른 밝기(shade)를 빼면 어느 쪽이 "머리"인지 안 보여요.
//    ② SpinVS 에서 회전을 끄기:  float2 r = Rotate(v.position, gCosSin);  ->  float2 r = v.position;
//       C++ 은 여전히 각도를 보내지만, 셰이더가 안 쓰면 아무 일도 없어요.
//    ③ 가방보다 많이 읽게 하기 (패널의 "b0 크기 검사"를 보면서)
//       (1) cbuffer 맨 끝에  float4 gExtra;  한 줄 추가하고 저장
//           -> "읽는 양"은 48바이트 그대로예요. 아무도 안 읽는 값은 컴파일러가 없는 셈 쳐요.
//       (2) SpinPS 의 return 을  return float4(gColor.rgb * i.shade + gExtra.rgb, 1.0);  로 바꾸고 저장
//           -> "읽는 양" 64바이트. A 에 빨간 경고: 가방(루트 상수 12개 = 48바이트)을 16바이트 넘어요.
//              그래도 PSO 는 셋 다 "됨"이에요. D3D12 는 크기가 모자란 건 막아 주지 않거든요.
//              A 의 넘친 16바이트는 아무도 정하지 않은 값이라 GPU 에 따라 색이 달라질 수도 있어요.
//              B, C 는 256바이트 칸 안이라 괜찮아요 (C++ 이 안 쓴 자리라 0 -> 색 그대로).
//       (실험이 끝나면 되돌리거나 "처음 코드로")
// =====================================================================

// 삼각형 하나에 보내는 값 12개 (float 12개 = 48바이트). C++ 의 SpinConstants 와 순서가 같아야 해요.
// HLSL 의 cbuffer 는 16바이트(float 4개) 단위 줄에 값을 채워요. float2 두 개가 한 줄, float4 하나가 한 줄.
cbuffer SpinConstants : register(b0)
{
    float2 gCosSin;   // 회전: (cos 각도, sin 각도). C++ 이 계산해서 보내요
    float2 gOffset;   // 삼각형 중심 위치 (그림 영역 기준 -1..1)
    float2 gScale;    // 크기. x 는 화면 가로세로 비율만큼 줄여서 찌그러지지 않게
    float2 gPad;      // 빈칸 (한 줄 16바이트를 채우려고)
    float4 gColor;    // 삼각형 색
};

struct VSOut
{
    float4 pos   : SV_Position;
    float  shade : SHADE;   // 꼭짓점 밝기 (머리 1.0, 나머지 0.55) -> 픽셀마다 섞여서 들어와요
};

// 점 p 를 (cos, sin) 만큼 돌리기
float2 Rotate(float2 p, float2 cs)
{
    return float2(cs.x * p.x - cs.y * p.y,
                  cs.y * p.x + cs.x * p.y);
}

// [회전-⑦] 정점 하나 = 12바이트 (C++ 의 SpinVertex, 입력 레이아웃과 짝). 게임 방식
struct SpinVertex
{
    float2 position : POSITION;   // 꼭짓점 위치 (정삼각형, 무게중심이 (0, 0) 이라 돌려도 제자리)
    float  shade    : SHADE;      // 꼭짓점 밝기 (0번 "머리" 1.0, 나머지 0.55)
};

// 게임 방식: IA 가 정점 버퍼에서 12바이트씩 잘라 v 에 넣어 줘요. 삼각형 3개가 같은 정점 버퍼를 같이 쓰고,
// 다른 건 b0 의 값(회전, 위치, 크기, 색)뿐이에요.
VSOut SpinVS(SpinVertex v)
{
    float2 r = Rotate(v.position, gCosSin);
    VSOut o;
    o.pos   = float4(r * gScale + gOffset, 0.0, 1.0);
    o.shade = v.shade;
    return o;
}

// 비교용: 정점 버퍼 없이 번호(0, 1, 2)로 꼭짓점을 만들어요 (장면 3과 같은 방법)
VSOut SpinVSNoBuffer(uint id : SV_VertexID)
{
    // 위 정점 버퍼와 같은 값을 셰이더 안에 직접 적어 둬요
    float2 p = float2(0.0, 1.0);  float shade = 1.0;
    if (id == 1) { p = float2( 0.866, -0.5); shade = 0.55; }
    if (id == 2) { p = float2(-0.866, -0.5); shade = 0.55; }

    float2 r = Rotate(p, gCosSin);
    VSOut o;
    o.pos   = float4(r * gScale + gOffset, 0.0, 1.0);
    o.shade = shade;
    return o;
}

float4 SpinPS(VSOut i) : SV_Target
{
    return float4(gColor.rgb * i.shade, 1.0);
}
