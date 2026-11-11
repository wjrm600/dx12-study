// =====================================================================
//  scene.h  -  "장면" 하나 = 공부한 주제 하나
//
//  새 주제를 배우면 scenes/ 폴더에 scene_xxx.cpp 를 하나 만들고
//  아래 Scene 을 상속해서 채운 뒤, main.cpp 의 kScenes 목록에 한 줄 추가한다.
//  (README.md 의 "새 장면 추가하는 법" 참고)
//
//  한 프레임에서 불리는 순서:
//    1) OnFrameReused(slot) : 이 프레임 자원 세트를 GPU가 다 썼다. 되읽기 결과를 꺼내도 된다.
//    2) OnPanel()           : 오른쪽 패널 안에 설정/설명 UI를 그린다 (ImGui)
//    3) OnCanvas(canvas)    : 왼쪽 그림 영역에서 마우스 처리 + ImGui 선/글자 그리기
//    4) OnRender(cl, canvas): DX12 명령을 기록한다 (진짜 GPU 그리기)
//    그 다음 core 가 ImGui 를 위에 덮어 그리고 Present 한다.
// =====================================================================
#pragma once
#include "core.h"

class Scene
{
public:
    virtual ~Scene() {}
    virtual const char* Name() const = 0;          // 메뉴에 보이는 이름
    virtual void Init() {}                          // GPU 자원 만들기 (장면을 고를 때 한 번)
    virtual void Shutdown() {}                      // GPU 자원 지우기 (다른 장면으로 바꿀 때)
    virtual void OnFrameReused(UINT /*slot*/) {}
    virtual void OnPanel() {}
    virtual void OnCanvas(const Canvas& /*c*/) {}
    virtual void OnRender(ID3D12GraphicsCommandList* /*cl*/, const Canvas& /*c*/) {}
    virtual void OnMinimized(const Canvas& /*c*/) {}  // 메인 창이 최소화된 동안 (독립 창만 그릴 때)

    // 개발용 자동 테스트 (환경변수 LAB_AUTOTEST=1). 매 프레임 불린다.
    // true 를 돌려주면 테스트 끝. failures 에 실패 개수를 더한다.
    virtual bool AutoTest(int /*frame*/, int& /*failures*/) { return true; }
};

// 장면 만들기 함수들 (scenes/*.cpp 에 있음)
Scene* CreateRasterScene();
