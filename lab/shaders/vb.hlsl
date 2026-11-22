// =====================================================================
//  vb.hlsl  -  장면 5 (정점 버퍼: 점을 GPU 에 넘기는 실제 게임 방식) 의 셰이더
//
//  정점 셰이더가 두 가지예요:
//    VSMain        : 정점 버퍼에서 IA 가 꺼내 준 점을 받는다 (방법 A 게임 방식, 방법 B 최소 방식)
//    VSNoBufferTri : 정점 버퍼 없이 번호(SV_VertexID)로 점을 만든다 (방법 C, 비교용)
//    VSNoBufferQuad: 위와 같은데 사각형 (삼각형 2개 = 점 6개)
//  픽셀 셰이더는 PSMain 하나를 같이 써요.
//
//  함수 이름(VSMain, VSNoBufferTri, VSNoBufferQuad, PSMain)은 C++ 이 찾으니까 바꾸지 마세요.
//  망가뜨려도 괜찮아요. 셰이더 창의 "처음 코드로" 버튼으로 돌아가요.
//
//  해 볼 실험
//    ① PSMain 을  return float4(1.0, 1.0, 1.0, 1.0);  로 -> 하얀 도형. 색은 정점 버퍼의 COLOR 에서 왔다는 뜻
//    ② VSInput 의  COLOR  를  COLOUR  로 바꾸고 저장 -> A, B 만 PSO 실패.
//       C++ 입력 레이아웃의 "COLOR" 와 이름(시맨틱)이 안 맞아서 IA 가 어디에 넣을지 모르거든요.
//       C 는 정점 버퍼를 안 쓰니까 그대로 돼요. (실험 끝나면 되돌리기)
//    ③ VSMain 에서  o.position = float4(input.position, 1.0);
//       ->  o.position = float4(input.position * 0.5, 1.0);  -> 도형이 반으로. 정점 버퍼는 그대로, 셰이더가 바꾼 것
// =====================================================================

// [정점-①] 정점 하나 = 28바이트. C++ 의 struct Vertex 와 입력 레이아웃이 이 모양을 설명해요.
//   POSITION : float3 (12바이트, 0번째 바이트부터)
//   COLOR    : float4 (16바이트, 12번째 바이트부터)
// 이름(POSITION, COLOR)이 "시맨틱"이에요. C++ 입력 레이아웃의 SemanticName 과 글자가 같아야 해요.
struct VSInput
{
    float3 position : POSITION;
    float4 color    : COLOR;
};

// VS 가 내보내는 것. SV_POSITION 은 시스템 값(래스터라이저가 읽는 화면 위치),
// COLOR 는 우리가 지은 이름(사용자 정의)이라 래스터라이저가 픽셀마다 섞어서 PS 에 넘겨요.
struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

// 방법 A, B: IA 가 정점 버퍼에서 28바이트씩 잘라 VSInput 에 채워 줘요. 점 하나에 한 번 실행.
PSInput VSMain(VSInput input)
{
    PSInput o;
    o.position = float4(input.position, 1.0);
    o.color    = input.color;
    return o;
}

// ---- 비교용: 정점 버퍼 없이 (방법 C) -------------------------------------
// 점 위치를 셰이더 안에 직접 적어 둬요. 장면 2, 3 이 쓰던 방법.
// 모양을 바꾸려면 셰이더를 고쳐야 해서, 모델을 파일에서 읽는 게임에서는 못 써요.
PSInput VSNoBufferTri(uint id : SV_VertexID)
{
    PSInput o;
    o.position = float4(0.0, 0.5, 0.0, 1.0);   o.color = float4(1, 0, 0, 1);   // 0 위 (빨강)
    if (id == 1) { o.position = float4( 0.5, -0.5, 0.0, 1.0); o.color = float4(0, 1, 0, 1); }   // 1 오른쪽 아래 (초록)
    if (id == 2) { o.position = float4(-0.5, -0.5, 0.0, 1.0); o.color = float4(0, 0, 1, 1); }   // 2 왼쪽 아래 (파랑)
    return o;
}

// 사각형 = 삼각형 2개 = 점 6개. 인덱스 버퍼가 없으니 왼쪽 위, 오른쪽 아래를 두 번씩 적어요.
PSInput VSNoBufferQuad(uint id : SV_VertexID)
{
    // 0 왼쪽 위, 1 오른쪽 위, 2 오른쪽 아래 | 3 왼쪽 위, 4 오른쪽 아래, 5 왼쪽 아래
    PSInput o;
    o.position = float4(-0.5, 0.5, 0.0, 1.0);  o.color = float4(1, 0, 0, 1);
    if (id == 1)             { o.position = float4( 0.5,  0.5, 0.0, 1.0); o.color = float4(0, 1, 0, 1); }
    if (id == 2 || id == 4)  { o.position = float4( 0.5, -0.5, 0.0, 1.0); o.color = float4(0, 0, 1, 1); }
    if (id == 5)             { o.position = float4(-0.5, -0.5, 0.0, 1.0); o.color = float4(1, 1, 0, 1); }
    return o;
}

// 픽셀마다 한 번. 래스터라이저가 세 꼭짓점 색을 섞어서 넘겨 준 color 를 그대로 칠해요.
float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
