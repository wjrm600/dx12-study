# 코드 읽기 안내 (시나리오별 지도)

## 먼저 결론: 전부 읽을 필요 없어요

`src/main.cpp`는 약 2600줄이지만, **발표 내용과 직접 관련된 줄은 시나리오마다 5~15줄, 다 합쳐도 150줄 정도**입니다. 나머지는 창 만들기, ImGui 패널, 측정, 테스트용 코드입니다.

발표 준비 단계에서는 이 정도면 충분해요.

1. 시나리오마다 **"최악일 때 실행되는 줄"과 "최적일 때 실행되는 줄"** 이 어디서 갈라지는지만 봅니다 (보통 `if` 하나).
2. 그 안에서 **DX12 함수 이름 하나**를 기억합니다 (`CopyBufferRegion`, `ResourceBarrier`, `DrawIndexedInstanced`, `Present` ...).
3. 청중이 "코드로는 어떻게 생겼어요?"라고 물으면 그 몇 줄만 보여 줍니다.

나중에 DX12를 직접 짜 볼 때 `LoadPipeline`(장치, 큐, 스왑체인 만들기)을 읽으면 됩니다.

2026-11-09 리팩토링: DirectX-Graphics-Samples 처럼 긴 함수를 단계별 작은 함수로 나눴어요 (동작은 그대로, 자동 테스트 141프레임의 숫자가 전후로 똑같은 것까지 확인). 그래서 이제 **함수 이름으로 찾는 게 제일 빨라요.**

## 찾는 법

Visual Studio에서 **Ctrl+F → `[시나리오 1]`** 처럼 검색하세요. 코드 안에 이런 표지판 주석을 달아 두었습니다.

데모를 켜면 패널의 각 설정 아래에도 `코드: s.변수 = 지금 값 · [시나리오 N] 쓰는 곳` 이 보입니다. 슬라이더를 움직이면 값이 바로 바뀌니, "이 슬라이더가 코드의 어떤 변수인지"를 화면에서 확인하고 그 `[시나리오 N]` 을 Ctrl+F로 찾으면 됩니다. 모든 설정 변수는 `struct Settings` 한 곳에 모여 있고, 코드에서는 `s` 라는 이름으로 씁니다.

```cpp
// ===== [시나리오 1] 최악: 매 프레임 CPU 원본 전체를 다시 memcpy =====
```

줄 번호 대신 함수 이름을 적어 뒀어요. Visual Studio 에서 함수 이름을 Ctrl+F 하거나, 위쪽 드롭다운(멤버 목록)에서 고르면 바로 가요.

## 한 프레임의 큰 흐름 (`RenderFrame`)

쉽게 말하면 **식당의 한 턴**입니다. 주문 확인 → 재료 준비 → 요리 지시서 쓰기 → 주방에 넘기기 → 손님에게 내기.
`RenderFrame` 은 이제 아래 함수들을 순서대로 부르기만 해요 (15줄). 단계마다 같이 쓰는 값은 `struct FrameCtx` 에 모여 있어요.

| 단계 | 함수 | 하는 일 | 관련 시나리오 |
|---|---|---|---|
| 0) 시작 | `BeginFrameTiming`, `ApplyPendingRebuilds` | 프레임 시간 재기, 설정 바뀐 것 다시 만들기 (메시, 텍스처, 스왑체인, 압박) | 1, 8, 9, 10 |
| 1) 펜스 대기 | `WaitForFrameSlot` | GPU가 너무 밀렸으면 기다림 | 4 |
| 1-2) 지난 결과 | `ReadFinishedFrameResults` | GPU 시간, 통계, 늦게 읽는 되읽기 | 7 |
| 2) UI | `BuildUi` → `DrawPanelAndApply`, `DrawBigText`, `DrawGraphOverlay` | 오른쪽 패널 (읽지 않아도 됨) | - |
| 3) CPU 데이터 준비 | `PrepareCpuData` → `WriteFrameConstants`, `SimulateGameLogic`, `ComputeCubePositions`, `UploadCubeInstances`, `PrepareMeshForFrame`, `PreparePsosForFrame`, `CreateTextureForFrame` | 큐브 위치 계산, 버퍼에 쓰기, 메시 재전송, 텍스처 새로 만들기 | 1, 2, 4, 6, 10 |
| 4) 명령 기록 | `PopulateCommandList` → `BeginCommandList`, `RecordTextureRecreate`, `DrawPlanet`, `DrawCubes`(`DrawCubesInstanced` / `DrawCubesOneByOne`), `TouchPressureChunks`, `CopySceneToReadback`, `RecordUiAndClose` | GPU에게 줄 "할 일 목록" 작성 | 1, 3, 5, 6, 7, 9, 10 |
| 5) 제출 + Present | `SubmitAndPresent`, `WaitReadbackNow`, `UpdateStatsAndHistory` | 목록을 GPU에 넘기고 화면 교체, 통계 | 7, 8 |

`PopulateCommandList` 라는 이름은 DirectX-Graphics-Samples(HelloTriangle 등)와 같아요. Samples 를 읽을 때와 같은 자리를 보면 됩니다.

## 시나리오별 지도

### 1. 정점 데이터 위치

- **비유**: 재료(정점)를 집 냉장고(Upload 힙)에 둘까, 주방 냉장고(Default 힙)에 둘까.
- **어디서 갈라지나**: `RebuildMesh` → `CreateMeshInDefaultHeap` / `CreateMeshInCpuWritableHeap`. 설정을 바꿀 때 한 번 실행되어 버퍼를 만듭니다.

```cpp
// 최적: Default 힙(VRAM)에 만들고, 임시 Upload 버퍼에서 복사 큐로 딱 한 번 복사 (CreateMeshInDefaultHeap)
g_meshStatic = CreateBuffer(HK_DEFAULT, size, false, &hr);
memcpy(staging->mapped, g_meshCpu.data(), size);          // CPU -> Upload 힙
RunOnCopyQueue([&](ID3D12GraphicsCommandList* cl) {
    cl->CopyBufferRegion(g_meshStatic->res.Get(), 0, staging->res.Get(), 0, size);  // Upload -> Default
});
```

```cpp
// 최악: 매 프레임 CPU 원본 전체를 다시 복사 (PrepareMeshForFrame)
memcpy(fr.meshFrame->mapped, g_meshCpu.data(), meshBytes);
```

- **반복 그리기** (`DrawPlanet`): 같은 정점 버퍼로 `DrawInstanced`를 N번 부릅니다.

```cpp
for (int r = 0; r < s.meshRepeat; r++) cl->DrawInstanced((UINT)g_meshCpu.size(), 1, 0, 0);
```

- **기억할 함수**: `CreateCommittedResource`(힙 종류 지정), `Map`/`memcpy`, `CopyBufferRegion`.

### 2. 리소스 생성

- **비유**: 택배 상자를 매번 공장에 주문할까, 있는 상자를 다시 쓸까.
- **어디서 갈라지나**: `UploadCubeInstances` 의 `if (s.bufferMode == BM_REUSE)`

```cpp
if (s.bufferMode == BM_REUSE)                                        // 최적
    memcpy(fr.inst->mapped, inst.data(), sizeof(Instance) * count); // 이미 있는 버퍼에 덮어쓰기만
else                                                                 // 최악
    ResPtr b = MustBuffer(HK_UPLOAD, ...);                          // 매 프레임 새로 만들기 (CreateCommittedResource)
    fr.trash.push_back(std::move(b));                               // GPU가 다 쓸 때까지 못 지워서 쌓아 둠
```

- **기억할 것**: 만들기는 비싸고, 버퍼 하나는 최소 64KB. 그래서 큰 버퍼 하나를 나눠 쓰고 재사용합니다.

### 3. 배리어

- **비유**: 공사장 신호수. 깃발이 오르면 모두 멈춤.
- **어디**: `RecordUselessBarrier` (부르는 곳: `DrawCubesInstanced`, `DrawCubesOneByOne`, `DrawCubes`)

```cpp
// 깊이 버퍼: 깊이 쓰기 -> 셰이더 읽기 -> 깊이 쓰기 (아무도 안 읽는데 왕복)
Transition(g_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
cl->ResourceBarrier(1, &x);
Transition(g_depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
cl->ResourceBarrier(1, &y);
```

- **꼭 필요한 배리어의 예**도 같은 파일에 있어요: 백 버퍼를 `PRESENT → RENDER_TARGET`(그리기 전), `RENDER_TARGET → PRESENT`(보여 주기 전)로 바꾸는 줄. `Transition(back,` 으로 검색하세요.

### 4. CPU-GPU 동기화

- **비유**: 펜스는 "여기까지 끝났어" 번호표. 번호표를 보고 너무 앞서 갔으면 기다립니다.
- **어디**: `WaitForFrameSlot`

```cpp
// "동시에 진행할 프레임 수"가 1이면 바로 앞 프레임, 2면 두 프레임 전 것이 끝날 때까지 기다림
if (g_frameNo >= s.framesInFlight)
    WaitFence(g_fence.Get(), g_frameFence[(g_frameNo - s.framesInFlight) % 8]);
```

- 짝이 되는 줄: `SubmitAndPresent` 의 `g_directQueue->Signal(g_fence.Get(), ++g_fenceValue);` (GPU가 이 지점을 지나면 번호표를 올림).
- **게임 로직 흉내**는 `SimulateGameLogic`. 그냥 CPU를 일부러 바쁘게 만드는 반복문입니다.

### 5. 드로우콜 / 인스턴싱

- **비유**: 한 명씩 전화 vs 단체 문자.
- **어디**: `DrawCubes` 의 `bool individual = ...` → 최적 `DrawCubesInstanced` / 최악 `DrawCubesOneByOne`

```cpp
// 최적: 큐브 N개를 명령 1번으로
cl->DrawIndexedInstanced(36, count, 0, 0, 0);       // 인덱스 36개 x 인스턴스 count개

// 최악: 큐브마다 명령 1번 (for 문 안)
cl->SetGraphicsRoot32BitConstants(0, 4, rc, 0);     // "너는 i번 큐브야"
cl->DrawIndexedInstanced(36, 1, 0, 0, 0);
```

- 셰이더 쪽(`kShaderSrc`)에서 `gInst[gBase + i.iid]` 가 "몇 번째 큐브인지"로 위치를 읽는 줄입니다.

### 6. PSO / 루트 시그니처

- **비유**: 사진 한 장마다 조명과 배경을 다시 세팅 vs 한 번 세팅하고 연달아 찍기.
- **어디 ①**: `DrawCubesOneByOne` 의 `if (s.resetStatePerDraw)` — 드로우마다 `SetCommonState`(루트 시그니처, 디스크립터 힙, 바인딩 5개, 뷰포트 ...)와 `SetPipelineState`(PSO A/B 번갈아)를 다시 부릅니다.
- **어디 ②**: `PreparePsosForFrame` 의 `if (s.recompilePso)` — 매 프레임 `D3DCompile` + `CreateGraphicsPipelineState`.
- **정석 위치**: `LoadAssets` 에서 PSO를 미리 만들어 둡니다 (`g_psoA = CreatePso(...)` 검색). PSO에 무엇이 들어가는지는 `CreatePso`를 보면 한눈에 보여요.

### 7. GPU → CPU 되읽기

- **비유**: 사진관에 필름 맡기고 앞에서 기다리기 vs 다음 주에 찾으러 가기.
- **복사**: `CopySceneToReadback` — 백 버퍼를 `COPY_SOURCE`로 바꾸고 `CopyTextureRegion`으로 Readback 힙에 복사.
- **최악**: `WaitReadbackNow` — 제출하자마자 `WaitFence(g_fence, g_fenceValue)`로 방금 프레임이 끝날 때까지 CPU가 멈춤.
- **좋은 방법**: `ReadFinishedFrameResults` 의 `if (fr.rbValid && s.readback == RB_DELAYED) ReadSceneColor(fr);` — 이 버퍼 세트가 3프레임 뒤에 다시 돌아왔을 때(이미 끝났을 때) 읽습니다.

### 8. 스왑체인 / Present

- **비유**: 그림판 여러 장을 돌려 쓰는 장치.
- **Present**: `SubmitAndPresent` 의 `g_swapChain->Present(s.vsync ? 1 : 0, ...)` — 첫 숫자가 1이면 모니터 교체 순간까지 기다림(VSync).
- **버퍼 개수**: `ResizeSwapChain` → `ResizeBuffers(count, ...)`.
- **처음 만드는 곳**: `CreateSwapChain` 안의 `CreateSwapChainForHwnd`.

### 9. VRAM 압박

- **비유**: 사물함 칸 수(예산)를 넘기면 짐이 집 창고로 밀려남.
- **할당**: `ApplyPressure` — 128MB 버퍼를 원하는 개수만큼 `CreateBuffer`.
- **Evict / MakeResident**: `SetPressureResidency` — `g_device->Evict(...)`, `g_device->MakeResident(...)` 두 줄이 핵심.
- **매 프레임 사용**: `TouchPressureChunks` — 덩어리에서 256바이트씩 `CopyBufferRegion` 해서 "이건 VRAM에 있어야 해"로 만듦.

### 10. 텍스처 (4K)

- **비유**: 엄지손톱만 한 그림을 그리려고 벽화 원본을 돋보기로 훑기(밉맵 없음) vs 미리 줄여 둔 엽서 보기(밉맵 있음). 매 프레임 새로 만들기는 매번 벽화를 새로 그려 배달시키는 것.
- **최적 (한 번만)**: `RebuildTexture` — 크기/밉맵 설정을 바꿀 때만 실행됩니다. 텍스처 한 장이 VRAM에 올라가는 7단계가 한 함수에 다 있어요.
```cpp
g_tex = CreateTracked(HK_DEFAULT, td, ...);                // 1) VRAM에 자리
staging = StageAndRecordTextureCopy(cl, g_tex, td);        // 2~4) Upload 임시 상자에 픽셀 쓰고 CopyTextureRegion
                                                           //      (RunOnCopyQueue = 복사 큐 + 펜스 대기)
staging.reset();                                           // 임시 상자 버리기
WriteTextureSrv(g_tex, g_texSrvCpu);                       // 5) 명함(SRV)
```
- **밉맵 만들기**: `GenerateTexturePixels` → `MakeCheckerPixels`(원본), `Downsample2x2`(2x2 픽셀 평균으로 절반 크기, 1x1까지 반복), `TintMipLevels`("밉 단계마다 색 칠하기").
- **줄 맞추기**: `StageAndRecordTextureCopy` — `GetCopyableFootprints`가 알려 주는 `RowPitch`(256바이트 배수)에 맞춰 한 줄씩 `memcpy` 합니다. 텍스처 업로드에서 제일 많이 틀리는 부분이에요.
- **최악 (매 프레임)**: `CreateTextureForFrame` 에서 `CreateTracked`로 새 텍스처를 만들고, `RecordTextureRecreate` 에서 픽셀 전체를 다시 쓰고 복사한 뒤 배리어(`COPY_DEST` → `PIXEL_SHADER_RESOURCE`)로 용도를 바꿉니다. 새 SRV는 프레임 세트 전용 자리에 써서, GPU가 아직 읽는 SRV를 덮어쓰지 않게 합니다.
- **셰이더에서 읽기**: `kShaderSrc` 의 `gTex.Sample(gSamp, i.uv)` — 샘플러가 `MIN_MAG_MIP_LINEAR`라서 밉맵이 있으면 GPU가 알아서 작은 단계를 골라요.

### 메모리 숫자는 어디서 오나

- **OS에게 묻는 곳**: `QueryMemory` — `QueryVideoMemoryInfo`(VRAM, 공유 GPU 메모리), `GlobalMemoryStatusEx`(시스템 RAM), `GetProcessMemoryInfo`(우리 프로그램).
- **힙별 MB**: `CreateTracked` — 모든 리소스를 여기서 만들면서 `GetResourceAllocationInfo`로 실제 크기를 더합니다.

## 지금은 건너뛰어도 되는 부분

| 부분 | 이유 |
|---|---|
| `DrawPanel` → `PanelPresets`, `PanelPerformance`, `PanelMemory`, `PanelScenario1_MeshHeap` ... `PanelScenario10_Texture` | 오른쪽 패널 UI. 시나리오마다 함수 하나, 툴팁 글이 대부분 |
| `AutotestStep`, `AutotestLogStats`, `DEMO_TEST_WINE_SHADER` | 제가 가상 환경에서 테스트하려고 넣은 코드 |
| `WinMain` → `CreateMainWindow`, `InitImGui`, `RunMessageLoop`, `Shutdown` / `WndProc` | 창 만들기 (Win32 기본) |
| 수학 함수 (`Mat4`, `LookAtLH` ...) | 카메라 행렬 계산 |
| `GenerateSphere`, `CreateCube` | 모양 만들기 |
| 타임스탬프 / 파이프라인 통계 쿼리 | 측정용 (GPU ms, 정점 수) |

## 나중에 DX12를 직접 짜 볼 때 읽는 순서

1. `InitD3D` = `LoadPipeline` + `LoadAssets` (Samples 와 같은 이름). `LoadPipeline` 은 `CreateDevice` → `QueryFeatures` → `CreateQueuesAndFences` → `CreateSwapChain` → `CreateDescriptorHeaps` → `CreateFrameResources` → `CreateQueryHeaps`. DX12 프로그램의 뼈대입니다.
2. `CreateRootSignature`, `CreatePso`: 셰이더에게 "자료는 이 칸으로 줄게"와 "이 설정으로 그려"를 정하는 곳.
3. `PopulateCommandList`: 실제 명령 기록 (`ResourceBarrier`, `Clear`, `Set...`, `Draw...`).
4. 셰이더 문자열 `kShaderSrc`: `register(b0)`, `register(t0)` 이 루트 시그니처의 칸과 어떻게 짝을 이루는지.
