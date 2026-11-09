// =====================================================================
//  shader_file.cpp  -  .hlsl 파일 읽기 / 핫 리로드 / 셰이더 편집 창
// =====================================================================
#include "shader_file.h"
#include "imgui.h"
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

ImFont* g_codeFont = nullptr;   // main.cpp 에서 고정폭 글꼴(Consolas + 한글)을 넣어 둔다

static const size_t kEditCapacity = 256 * 1024;   // 편집 창 최대 256KB

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static uint64_t LastWrite(const std::wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}
static bool ReadAll(const std::wstring& p, std::string& out)
{
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);   // UTF-8 BOM 제거
    return true;
}

// shaders 폴더 찾기:
//  1) 소스 폴더의 lab/shaders (CMake 가 LAB_SHADER_DIR 로 알려 줌) -> 고치면 git 저장소의 파일이 바로 바뀜
//  2) exe 옆 shaders 폴더 (빌드할 때 복사됨) -> exe 만 다른 PC 로 옮겼을 때
static std::wstring FindShader(const char* fileName)
{
    std::vector<std::wstring> dirs;
#ifdef LAB_SHADER_DIR
    dirs.push_back(Widen(LAB_SHADER_DIR));
#endif
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exeDir(exe); exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/") + 1);
    dirs.push_back(exeDir + L"shaders\\");
    dirs.push_back(L"shaders\\");
    for (auto& d : dirs)
    {
        std::wstring p = d + Widen(fileName);
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return std::wstring();
}

bool ShaderFile::Open(const char* fileName)
{
    name = fileName;
    path = FindShader(fileName);
    if (path.empty())
    {
        error = std::string("셰이더 파일을 못 찾았어요: ") + fileName + "\nlab/shaders 폴더나 exe 옆 shaders 폴더에 있어야 해요.";
        return false;
    }
    pathUtf8 = Narrow(path);
    m_lastWrite = LastWrite(path);
    if (!ReadAll(path, source)) { error = "셰이더 파일을 읽지 못했어요: " + pathUtf8; return false; }
    ReloadEditFromSource();
    return true;
}

void ShaderFile::ReloadEditFromSource()
{
    edit.assign(kEditCapacity, '\0');
    size_t n = std::min(source.size(), kEditCapacity - 1);
    memcpy(edit.data(), source.data(), n);
    editDirty = false;
}

bool ShaderFile::NeedsRebuild()
{
    if (path.empty()) return false;
    double now = NowMs();
    bool changed = false;
    if (m_force || now - m_lastCheck > 300.0)   // 0.3초마다 파일이 바뀌었는지 확인
    {
        m_lastCheck = now;
        uint64_t w = LastWrite(path);
        if (w != 0 && w != m_lastWrite) changed = true;
        if (changed || m_force)
        {
            std::string s;
            if (ReadAll(path, s))
            {
                m_lastWrite = w;
                source = s;
                if (!editDirty) ReloadEditFromSource();   // 편집 중인 내용은 덮어쓰지 않음
                changed = true;
            }
        }
    }
    m_force = false;
    return changed;
}

bool ShaderFile::SaveEdit()
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { error = "저장 실패: " + pathUtf8; return false; }
    size_t n = strlen(edit.data());
    fwrite(edit.data(), 1, n, f);
    fclose(f);
    editDirty = false;
    m_force = true;   // 다음 프레임에 바로 다시 컴파일
    return true;
}

bool TryCompile(const ShaderFile& f, const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& err)
{
    ComPtr<ID3DBlob> code, msg;
    HRESULT hr = D3DCompile(f.source.c_str(), f.source.size(), f.name.c_str(), nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &msg);
    if (FAILED(hr))
    {
        err += std::string("[") + entry + "] ";
        err += msg ? std::string((const char*)msg->GetBufferPointer(), msg->GetBufferSize()) : std::string("컴파일 실패");
        while (!err.empty() && (err.back() == '\0' || err.back() == '\n' || err.back() == '\r')) err.pop_back();
        err += "\n";
        return false;
    }
    out = code;
    return true;
}

// 에러 메시지에서 "raster.hlsl(57,12" 의 57 같은 줄 번호를 모은다
static std::vector<int> ErrorLines(const ShaderFile& f)
{
    std::vector<int> lines;
    std::string key = f.name + "(";
    size_t pos = 0;
    while ((pos = f.error.find(key, pos)) != std::string::npos)
    {
        pos += key.size();
        int ln = atoi(f.error.c_str() + pos);
        if (ln > 0 && std::find(lines.begin(), lines.end(), ln) == lines.end()) lines.push_back(ln);
    }
    return lines;
}

static int CursorCallback(ImGuiInputTextCallbackData* d)
{
    ShaderFile* f = (ShaderFile*)d->UserData;
    int line = 1;
    for (int i = 0; i < d->CursorPos && i < d->BufTextLen; i++) if (d->Buf[i] == '\n') line++;
    f->cursorLine = line;
    return 0;
}

void DrawShaderEditor(ShaderFile& f, const Canvas& c)
{
    ImGuiIO& io = ImGui::GetIO();
    // 단축키: F2 창 열기/닫기, F5 파일에서 다시 읽고 컴파일
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) f.editorOpen = !f.editorOpen;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) f.RequestRebuild();
    if (!f.editorOpen) return;

    ImGui::SetNextWindowPos(ImVec2(c.x + c.w * 0.40f, c.y + c.h * 0.30f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(c.w * 0.58f, c.h * 0.68f), ImGuiCond_FirstUseEver);
    std::string title = "셰이더: " + f.name + (f.editDirty ? " *" : "") + "###shader_editor_" + f.name;
    if (!ImGui::Begin(title.c_str(), &f.editorOpen)) { ImGui::End(); return; }

    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (ImGui::Button("저장하고 컴파일 (Ctrl+S)") || (focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))) f.SaveEdit();
    ImGui::SameLine();
    if (ImGui::Button("파일에서 다시 읽기 (F5)")) { f.editDirty = false; f.RequestRebuild(); }
    ImGui::SameLine();
    if (ImGui::Button("메모장으로 열기")) ShellExecuteW(nullptr, L"open", L"notepad.exe", f.path.c_str(), nullptr, SW_SHOWNORMAL);
    ImGui::TextDisabled("%s", f.pathUtf8.c_str());

    // 상태 줄
    if (!f.error.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("컴파일 에러! 화면은 마지막으로 성공한 셰이더로 계속 그려요.");
        ImGui::TextWrapped("%s", f.error.c_str());
        ImGui::PopStyleColor();
    }
    else
        ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.55f, 1.0f), "컴파일 성공 (%d번째)%s", f.buildCount,
                           f.editDirty ? "  ·  고친 내용은 아직 저장 안 함" : "  ·  파일을 저장하면 자동으로 다시 컴파일돼요");

    std::vector<int> errLines = ErrorLines(f);
    if (ImGui::BeginTabBar("tabs"))
    {
        if (ImGui::BeginTabItem("편집"))
        {
            ImGui::TextDisabled("줄 %d", f.cursorLine);
            if (g_codeFont) ImGui::PushFont(g_codeFont, 0.0f);
            if (ImGui::InputTextMultiline("##code", f.edit.data(), f.edit.size(), ImVec2(-1, -1),
                                          ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways, CursorCallback, &f))
                f.editDirty = true;
            if (g_codeFont) ImGui::PopFont();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("줄 번호로 보기"))
        {
            ImGui::TextDisabled("에러가 난 줄은 빨간색이에요. (보기 전용)");
            ImGui::BeginChild("lines", ImVec2(-1, -1), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            if (g_codeFont) ImGui::PushFont(g_codeFont, 0.0f);
            const char* s = f.edit.data();
            int ln = 1;
            while (*s || ln == 1)
            {
                const char* e = strchr(s, '\n');
                size_t len = e ? (size_t)(e - s) : strlen(s);
                bool bad = std::find(errLines.begin(), errLines.end(), ln) != errLines.end();
                ImGui::TextColored(bad ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.5f, 0.5f, 0.55f, 1), "%4d", ln);
                ImGui::SameLine();
                std::string line(s, len);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (bad) ImGui::TextColored(ImVec4(1, 0.55f, 0.55f, 1), "%s", line.c_str());
                else ImGui::TextUnformatted(line.c_str());
                if (!e) break;
                s = e + 1; ln++;
            }
            if (g_codeFont) ImGui::PopFont();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
