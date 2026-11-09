// =====================================================================
//  shader_file.h  -  .hlsl 파일 읽기, 고치면 다시 컴파일(핫 리로드), 앱 안 셰이더 편집 창
//
//  비유: 요리하다가 레시피 카드(.hlsl)를 고치면, 주방을 껐다 켜지 않고
//        다음 접시부터 바로 새 레시피로 만드는 것.
//
//  장면에서 쓰는 법:
//    ShaderFile m_sh;
//    Init:      m_sh.Open("raster.hlsl");  그리고 m_sh.source 로 컴파일
//    매 프레임: if (m_sh.NeedsRebuild()) { 컴파일 -> 성공하면 PSO 교체, 실패하면 m_sh.error 에 넣기 }
//    UI:        DrawShaderEditor(m_sh);
// =====================================================================
#pragma once
#include "core.h"
#include <string>
#include <vector>

struct ImFont;
extern ImFont* g_codeFont;   // 셰이더 편집 창 글꼴 (없으면 기본 글꼴)

struct ShaderFile
{
    std::string  name;          // "raster.hlsl" (에러 메시지에 나오는 이름)
    std::wstring path;          // 디스크의 실제 위치
    std::string  pathUtf8;
    std::string  source;        // 지금 컴파일에 쓰는 내용
    std::string  error;         // 마지막 컴파일 에러 ("" = 성공)
    int          buildCount = 0;   // 성공한 컴파일 횟수
    bool         editorOpen = true;

    // 편집 창 버퍼
    std::vector<char> edit;
    bool         editDirty = false;   // 편집 창에서 고치고 아직 저장 안 함
    int          cursorLine = 1;

    bool Open(const char* fileName);   // shaders 폴더에서 찾아 읽기
    bool NeedsRebuild();               // 파일이 바뀌었거나 F5 를 눌렀으면 true (내용은 source 에 다시 읽어 둠)
    void RequestRebuild() { m_force = true; }
    bool SaveEdit();                   // 편집 창 내용을 파일에 저장 (-> 다음 프레임에 다시 컴파일)
    void ReloadEditFromSource();

    uint64_t m_lastWrite = 0;
    double   m_lastCheck = 0;
    bool     m_force = false;
    bool     m_changed = false;
};

// 실패해도 앱을 끄지 않는 컴파일. 실패하면 err 에 "raster.hlsl(12,5): error ..." 같은 메시지
bool TryCompile(const ShaderFile& f, const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& err);

// 셰이더 편집 창 (ImGui 창 하나). canvas 는 처음 위치를 잡을 때만 쓴다.
void DrawShaderEditor(ShaderFile& f, const Canvas& canvas);
