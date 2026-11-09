@echo off
chcp 65001 >nul
rem DX12 메모리 최적화 데모 - 빌드하고 바로 실행
rem Visual Studio 2026 / 2022 둘 다 자동으로 찾는다.
cd /d "%~dp0"

rem cmake가 PATH에 없으면(일반 cmd, 더블클릭) VS 개발자 환경을 직접 불러온다.
rem VS에는 CMake와 Ninja가 들어 있지만 PATH에 등록되지는 않는다.
where cmake >nul 2>nul
if not errorlevel 1 goto have_cmake
call :load_vs_env
cd /d "%~dp0"
where cmake >nul 2>nul
if not errorlevel 1 goto have_cmake
echo.
echo cmake 를 찾지 못했습니다. Visual Studio 설치 관리자에서
echo "C++를 사용한 데스크톱 개발" 워크로드가 설치돼 있는지 확인하세요.
goto fail
:have_cmake

rem 이전에 다른 VS 버전으로 구성한 흔적이 있으면 지운다
if exist build\CMakeCache.txt (
  findstr /c:"CMAKE_GENERATOR:INTERNAL" build\CMakeCache.txt > build\_gen.txt
)

echo [1/3] Visual Studio 2026 로 구성 시도...
call :try "Visual Studio 18 2026" && goto build
echo [2/3] Visual Studio 2022 로 구성 시도...
call :try "Visual Studio 17 2022" && goto build
echo [3/3] Ninja 로 구성 시도...
if exist build rmdir /s /q build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto fail
cmake --build build
if errorlevel 1 goto fail
start "" "build\DX12MemoryDemo.exe"
goto end

:try
if exist build\_gen.txt (
  findstr /c:%1 build\_gen.txt >nul || rmdir /s /q build
)
cmake -S . -B build -G %1 -A x64
if errorlevel 1 (
  if exist build rmdir /s /q build
  exit /b 1
)
exit /b 0

:load_vs_env
rem vswhere로 C++ 도구가 설치된 최신 VS를 찾아 vcvars64.bat을 부른다 (cmake, ninja, cl 이 PATH에 잡힘)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
set "VSDIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR exit /b 1
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" exit /b 1
echo Visual Studio 개발자 환경을 불러옵니다: %VSDIR%
rem vcvars 내부 스크립트도 vswhere.exe를 PATH에서 찾으므로 Installer 폴더를 넣어 둔다
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0

:build
cmake --build build --config Release
if errorlevel 1 goto fail
start "" "build\Release\DX12MemoryDemo.exe"
goto end

:fail
echo.
echo 빌드 실패. README.md 의 "문제 해결"을 보세요.
pause
:end
