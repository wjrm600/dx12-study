// =====================================================================
//  mesh.h  -  정점 버퍼 · 인덱스 버퍼 (실제 게임에서 쓰는 방식)
//
//  모델의 꼭짓점처럼 "한 번 올려 두고 매 프레임 읽기만 하는" 데이터는
//  게임에서는 Default 힙(VRAM)에 둔다. GPU 가 가까운 자기 메모리에서 읽으니 빠르다.
//  그런데 CPU 는 Default 힙에 직접 쓸 수 없어서, 이렇게 두 번 옮긴다:
//
//    CPU 배열 --memcpy--> Upload 버퍼(RAM, 잠깐 쓰는 짐 칸) --GPU 복사--> Default 버퍼(VRAM)
//
//  비유: 이삿짐을 트럭(Upload)에 싣고, 트럭이 새 집(VRAM)에 내려 준 뒤 돌아간다.
//        트럭은 짐을 다 내릴 때까지(펜스) 돌려보내면 안 된다.
//
//  비교용 "최소 방식"(Microsoft HelloTriangle 과 같음)도 같이 있다:
//    Upload 힙에 버퍼 하나만 만들고 그대로 그린다. 코드는 짧지만,
//    GPU 가 그릴 때마다 PCIe 너머 RAM 에서 읽어서 큰 모델일수록 느리다.
//
//  코드에서 Ctrl+F 로 찾기 (전체 흐름은 scenes/scene_vertex.cpp 맨 위에 있음):
//    [정점-②] Default 버퍼 + Upload 짐 칸 만들기
//    [정점-③] CPU 가 Upload 짐 칸에 쓰기 (Map, memcpy, Unmap)
//    [정점-④] GPU 에게 복사 시키기 (CopyBufferRegion + 배리어)
//    [정점-⑤] 실행하고 펜스로 기다리기 (그 뒤에야 짐 칸을 지운다)
//    [정점-⑥] 뷰 만들기 (주소, 전체 크기, 한 칸 크기)
//    [정점-⑨] 비교용 최소 방식: Upload 힙 하나로 끝
// =====================================================================
#pragma once
#include "core.h"

// 게임 방식: data 를 Default 힙(VRAM) 버퍼에 올리고, finalState 로 바꿔서 돌려준다.
// 끝나면 GPU 복사까지 다 끝난 상태라 바로 그릴 수 있다 (안에서 펜스로 기다림).
//   정점 버퍼  : finalState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
//   인덱스 버퍼: finalState = D3D12_RESOURCE_STATE_INDEX_BUFFER
ComPtr<ID3D12Resource> CreateDefaultBuffer(const void* data, uint64_t size, D3D12_RESOURCE_STATES finalState, const wchar_t* name);

// 비교용 최소 방식: Upload 힙(RAM) 버퍼 하나에 data 를 쓰고 그대로 돌려준다.
ComPtr<ID3D12Resource> CreateUploadBufferWithData(const void* data, uint64_t size, const wchar_t* name);

enum class MeshHeap
{
    Default,   // 게임 방식: VRAM. 올릴 때 한 번 복사, 그 뒤엔 빠르게 읽음
    Upload,    // 최소 방식 (비교용): RAM. 복사 없음, 대신 그릴 때마다 PCIe 너머에서 읽음
};

// 정점 버퍼 + (있으면) 인덱스 버퍼 + 두 뷰를 한 묶음으로
struct Mesh
{
    ComPtr<ID3D12Resource>   vb, ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};          // 정점 버퍼 뷰: 주소, 전체 바이트, 정점 한 개 바이트(stride)
    D3D12_INDEX_BUFFER_VIEW  ibv{};          // 인덱스 버퍼 뷰: 주소, 전체 바이트, 번호 형식(R16_UINT)
    UINT                     vertexCount = 0;
    UINT                     indexCount = 0; // 0 이면 인덱스 버퍼 없이 DrawInstanced
    MeshHeap                 heap = MeshHeap::Default;

    bool Valid() const { return vb != nullptr; }
    UINT VertexBytes() const { return vbv.SizeInBytes; }
    UINT IndexBytes() const { return ibv.SizeInBytes; }

    // IA 에 버퍼를 꽂고 그린다. 토폴로지(IASetPrimitiveTopology)는 부르는 쪽이 정한다.
    void Draw(ID3D12GraphicsCommandList* cl, UINT instances = 1) const;
};

// indices 가 nullptr 이거나 indexCount 가 0 이면 인덱스 버퍼 없이 만든다.
Mesh CreateMesh(const void* vertices, UINT stride, UINT vertexCount,
                const uint16_t* indices, UINT indexCount,
                MeshHeap heap, const wchar_t* name);
