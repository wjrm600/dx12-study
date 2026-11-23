// =====================================================================
//  mesh.cpp  -  정점 버퍼 · 인덱스 버퍼 올리기 (설명은 mesh.h 맨 위)
// =====================================================================
#include "mesh.h"
#include <cstring>

// ===== [정점-②] 게임 방식: Default 버퍼(VRAM, 최종 자리) + Upload 짐 칸(RAM, 잠깐) =====
ComPtr<ID3D12Resource> CreateDefaultBuffer(const void* data, uint64_t size, D3D12_RESOURCE_STATES finalState, const wchar_t* name)
{
    // 버퍼는 만들 때 늘 COMMON 상태예요 (다른 상태를 적어도 COMMON 으로 만들어져요).
    auto dst    = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COMMON);
    // Upload 힙은 늘 GENERIC_READ 로 만들어요 (CPU 가 쓰고 GPU 가 읽는 곳).
    auto upload = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    dst->SetName(name);                    // PIX, 디버그 레이어 메시지에 보이는 이름
    upload->SetName(L"upload staging");

    // ===== [정점-③] CPU 가 Upload 짐 칸에 쓰기 =====
    void* p = nullptr;
    D3D12_RANGE noRead{ 0, 0 };            // "CPU 는 여기서 읽지 않아요" (쓰기만)
    HR(upload->Map(0, &noRead, &p));
    memcpy(p, data, (size_t)size);
    upload->Unmap(0, nullptr);

    // ===== [정점-④] GPU 에게 복사 시키기: Upload -> Default, 그다음 "이제 정점으로 읽을게" 배리어 =====
    // 프레임용 명령 목록(g_dx.list)과 섞이지 않게, 올리기 전용 명령 목록을 잠깐 만들어요.
    // (실제 게임은 여러 버퍼를 한 목록에 모아 한 번에 보내거나, 복사 전용 큐(COPY 큐)를 따로 써요)
    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    HR(g_dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    HR(g_dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)));

    auto toCopy = Transition(dst.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(1, &toCopy);
    cl->CopyBufferRegion(dst.Get(), 0, upload.Get(), 0, size);
    auto toRead = Transition(dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    cl->ResourceBarrier(1, &toRead);
    HR(cl->Close());

    // ===== [정점-⑤] 실행하고 펜스로 기다리기 =====
    // GPU 가 복사를 끝내기 전에 upload 를 지우면, GPU 는 이미 없는 메모리에서 읽게 돼요.
    // 그래서 WaitIdle(펜스 번호표를 걸고 기다림) 뒤에 함수를 나가요. 나갈 때 upload, alloc, cl 이 지워져요.
    ID3D12CommandList* lists[] = { cl.Get() };
    g_dx.queue->ExecuteCommandLists(1, lists);
    WaitIdle();
    return dst;
}

// ===== [정점-⑨] 비교용 최소 방식 (HelloTriangle 과 같음): Upload 힙 하나로 끝 =====
// 복사, 배리어, 펜스가 다 없어서 짧아요. 대신 이 버퍼는 RAM 에 있어서,
// GPU 가 그릴 때마다 PCIe 너머에서 읽어요. 삼각형 하나는 티가 안 나지만 정점 수십만 개면 느려져요.
ComPtr<ID3D12Resource> CreateUploadBufferWithData(const void* data, uint64_t size, const wchar_t* name)
{
    ComPtr<ID3D12Resource> buf = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    buf->SetName(name);
    void* p = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    HR(buf->Map(0, &noRead, &p));
    memcpy(p, data, (size_t)size);
    buf->Unmap(0, nullptr);
    return buf;
}

Mesh CreateMesh(const void* vertices, UINT stride, UINT vertexCount,
                const uint16_t* indices, UINT indexCount,
                MeshHeap heap, const wchar_t* name)
{
    Mesh m;
    m.heap = heap;
    m.vertexCount = vertexCount;
    m.indexCount = (indices && indexCount) ? indexCount : 0;

    const UINT vbBytes = stride * vertexCount;
    m.vb = heap == MeshHeap::Default
         ? CreateDefaultBuffer(vertices, vbBytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, name)
         : CreateUploadBufferWithData(vertices, vbBytes, name);

    // ===== [정점-⑥] 뷰 = "버퍼를 이렇게 읽어 줘" 쪽지: 어디서부터(주소), 얼마나(전체 크기), 몇 바이트씩(stride) =====
    m.vbv.BufferLocation = m.vb->GetGPUVirtualAddress();
    m.vbv.SizeInBytes    = vbBytes;
    m.vbv.StrideInBytes  = stride;          // 정점 한 개 크기. IA 가 이만큼씩 잘라서 VS 에 한 개씩 넘겨요

    if (m.indexCount)
    {
        const UINT ibBytes = (UINT)sizeof(uint16_t) * m.indexCount;
        m.ib = heap == MeshHeap::Default
             ? CreateDefaultBuffer(indices, ibBytes, D3D12_RESOURCE_STATE_INDEX_BUFFER, name)
             : CreateUploadBufferWithData(indices, ibBytes, name);
        m.ibv.BufferLocation = m.ib->GetGPUVirtualAddress();
        m.ibv.SizeInBytes    = ibBytes;
        m.ibv.Format         = DXGI_FORMAT_R16_UINT;   // 번호 하나 = 2바이트 (정점 65535개까지)
    }
    return m;
}

// ===== [정점-⑧] 그리기: IA 에 버퍼 꽂기 -> Draw =====
void Mesh::Draw(ID3D12GraphicsCommandList* cl, UINT instances) const
{
    cl->IASetVertexBuffers(0, 1, &vbv);     // 슬롯 0 에 정점 버퍼 뷰
    if (indexCount)
    {
        cl->IASetIndexBuffer(&ibv);
        cl->DrawIndexedInstanced(indexCount, instances, 0, 0, 0);   // 번호표 indexCount 개를 따라 정점을 꺼냄
    }
    else
    {
        cl->DrawInstanced(vertexCount, instances, 0, 0);            // 정점을 앞에서부터 차례대로
    }
}
