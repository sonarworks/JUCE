/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce::detail
{

namespace
{

class KeyedMutexSync : public VideoFrameSync
{
public:
    KeyedMutexSync (std::vector<Slot> slotsIn,
                    std::vector<ComSmartPtr<IDXGIKeyedMutex>> writerMutexesIn,
                    std::vector<ComSmartPtr<IDXGIKeyedMutex>> readerMutexesIn,
                    ComSmartPtr<ID3D11DeviceContext> writerContextIn)
        : slots (std::move (slotsIn)),
          writerMutexes (std::move (writerMutexesIn)),
          readerMutexes (std::move (readerMutexesIn)),
          writerContext (std::move (writerContextIn))
    {
    }

    ~KeyedMutexSync() override
    {
        // We release the key that the reader still holds on the displayed slot, because IDXGIKeyedMutex requires
        // every AcquireSync() to be paired with a ReleaseSync().
        if (displayed >= 0)
            readerMutexes[(size_t) displayed]->ReleaseSync (0);
    }

    int getSlotCount() const override                { return (int) slots.size(); }
    const Slot& getSlot (int index) const override   { return slots[(size_t) index]; }
    int getPending() const override                  { return pending; }

    int beginWrite() override
    {
        const auto count = getSlotCount();

        for (int step = 1; step <= count; ++step)
        {
            const auto candidate = (lastWritten + step) % count;

            if (candidate == displayed || candidate == pending)
                continue;

            // A zero timeout skips a slot the reader holds without blocking the message thread. Key 1 means the reader
            // never took the written slot, so we take it back ourselves, otherwise the ring would lose that slot.
            auto& writerMutex = *writerMutexes[(size_t) candidate];

            if (writerMutex.AcquireSync (0, 0) == S_OK || writerMutex.AcquireSync (1, 0) == S_OK)
                return candidate;
        }

        return -1;
    }

    void endWrite (int slot) override
    {
        lastWritten = slot;
        writerMutexes[(size_t) slot]->ReleaseSync (1);

        // We flush the writer, because the driver may hold the ReleaseSync() call in its command buffer. The reader's
        // acquire then fails, and if a newer frame replaces this one before a retry, this frame is lost.
        writerContext->Flush();
    }

    void cancelWrite (int slot) override
    {
        writerMutexes[(size_t) slot]->ReleaseSync (0);
    }

    bool setDisplayed (int slot) override
    {
        // We use a zero timeout, so this acquire never blocks the message thread. If it fails, the caller tries the
        // slot again. When a newer slot replaces this one as pending, beginWrite() takes this slot back with key 1.
        if (readerMutexes[(size_t) slot]->AcquireSync (1, 0) != S_OK)
        {
            pending = slot;
            return false;
        }

        if (displayed >= 0)
            readerMutexes[(size_t) displayed]->ReleaseSync (0);

        displayed = slot;
        pending = -1;
        return true;
    }

private:
    std::vector<Slot> slots;
    std::vector<ComSmartPtr<IDXGIKeyedMutex>> writerMutexes;
    std::vector<ComSmartPtr<IDXGIKeyedMutex>> readerMutexes;
    ComSmartPtr<ID3D11DeviceContext> writerContext;
    int displayed = -1;
    int pending = -1;
    int lastWritten = -1;
};

//==============================================================================
class SharedFenceSync : public VideoFrameSync
{
public:
    struct Parts
    {
        std::vector<Slot> slots;
        ComSmartPtr<ID3D11Fence> writeFence;           // on the writer device
        ComSmartPtr<ID3D11Fence> writeFenceOnReader;   // the same fence, opened on the reader device
        ComSmartPtr<ID3D11Fence> readFence;            // on the reader device, polled by the CPU
        ComSmartPtr<ID3D11DeviceContext4> writerContext;
        ComSmartPtr<ID3D11DeviceContext4> readerContext;
    };

    SharedFenceSync (Parts partsIn)
        : parts (std::move (partsIn)),
          writtenValue (parts.slots.size(), 0),
          reusableAfter (parts.slots.size(), 0)
    {
    }

    int getSlotCount() const override                { return (int) parts.slots.size(); }
    const Slot& getSlot (int index) const override   { return parts.slots[(size_t) index]; }
    int getPending() const override                  { return -1; }

    int beginWrite() override
    {
        const auto readDone = parts.readFence->GetCompletedValue();
        const auto count = getSlotCount();

        for (int step = 1; step <= count; ++step)
        {
            const auto candidate = (lastWritten + step) % count;

            if (candidate != displayed && readDone >= reusableAfter[(size_t) candidate])
                return candidate;
        }

        return -1;
    }

    void endWrite (int slot) override
    {
        lastWritten = slot;
        parts.writerContext->Signal (parts.writeFence, ++writeValue);

        // We flush the writer, because otherwise the signal could stay in its command buffer and the reader's
        // GPU queue would stall on its wait until something else flushed the writer.
        parts.writerContext->Flush();
        writtenValue[(size_t) slot] = writeValue;
    }

    void cancelWrite (int) override {}

    bool setDisplayed (int slot) override
    {
        parts.readerContext->Wait (parts.writeFenceOnReader, writtenValue[(size_t) slot]);

        if (displayed >= 0)
        {
            // Every reader command that drew the previous slot is queued before this signal,
            // so the slot can be written again once the read fence reaches this value.
            parts.readerContext->Signal (parts.readFence, ++readValue);
            reusableAfter[(size_t) displayed] = readValue;
        }

        displayed = slot;
        return true;
    }

private:
    Parts parts;
    std::vector<UINT64> writtenValue;
    std::vector<UINT64> reusableAfter;
    UINT64 writeValue = 0;
    UINT64 readValue = 0;
    int displayed = -1;
    int lastWritten = -1;
};

//==============================================================================
VideoFrameSyncResult fail (HRESULT result, const char* step)
{
    VideoFrameSyncResult failure;
    failure.result = result;
    failure.failedStep = step;
    return failure;
}

VideoFrameSyncResult createKeyedMutexSync (ID3D11Device& writerDevice,
                                           ID3D11Device& readerDevice,
                                           D3D11_TEXTURE2D_DESC desc,
                                           int slotCount)
{
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    std::vector<VideoFrameSync::Slot> slots;
    std::vector<ComSmartPtr<IDXGIKeyedMutex>> writerMutexes, readerMutexes;

    for (int i = 0; i < slotCount; ++i)
    {
        VideoFrameSync::Slot slot;

        if (const auto hr = writerDevice.CreateTexture2D (&desc, nullptr, slot.writerTexture.resetAndGetPointerAddress()); FAILED (hr))
            return fail (hr, "CreateTexture2D (keyed mutex)");

        ComSmartPtr<IDXGIResource> resource;

        if (const auto hr = slot.writerTexture.QueryInterface (resource); FAILED (hr))
            return fail (hr, "QueryInterface (IDXGIResource)");

        // A legacy shared handle is not a real handle, so we don't close it.
        HANDLE handle = nullptr;

        if (const auto hr = resource->GetSharedHandle (&handle); FAILED (hr))
            return fail (hr, "IDXGIResource::GetSharedHandle");

        if (const auto hr = readerDevice.OpenSharedResource (handle, __uuidof (ID3D11Texture2D),
                                                             (void**) slot.readerTexture.resetAndGetPointerAddress());
            FAILED (hr))
        {
            return fail (hr, "ID3D11Device::OpenSharedResource");
        }

        ComSmartPtr<IDXGIKeyedMutex> writerMutex, readerMutex;

        if (const auto hr = slot.writerTexture.QueryInterface (writerMutex); FAILED (hr))
            return fail (hr, "QueryInterface (IDXGIKeyedMutex, writer)");

        if (const auto hr = slot.readerTexture.QueryInterface (readerMutex); FAILED (hr))
            return fail (hr, "QueryInterface (IDXGIKeyedMutex, reader)");

        slots.push_back (slot);
        writerMutexes.push_back (writerMutex);
        readerMutexes.push_back (readerMutex);
    }

    ComSmartPtr<ID3D11DeviceContext> writerContext;
    writerDevice.GetImmediateContext (writerContext.resetAndGetPointerAddress());

    VideoFrameSyncResult result;
    result.sync = std::make_unique<KeyedMutexSync> (std::move (slots), std::move (writerMutexes), std::move (readerMutexes),
                                                    std::move (writerContext));
    return result;
}

VideoFrameSyncResult createSharedFenceSync (ID3D11Device& writerDevice,
                                            ID3D11Device& readerDevice,
                                            D3D11_TEXTURE2D_DESC desc,
                                            int slotCount)
{
    ComSmartPtr<ID3D11Device> writer { &writerDevice, IncrementRef::yes };
    ComSmartPtr<ID3D11Device> reader { &readerDevice, IncrementRef::yes };
    ComSmartPtr<ID3D11Device5> writer5, reader5;

    if (const auto hr = writer.QueryInterface (writer5); FAILED (hr))
        return fail (hr, "QueryInterface (ID3D11Device5, writer)");

    if (const auto hr = reader.QueryInterface (reader5); FAILED (hr))
        return fail (hr, "QueryInterface (ID3D11Device5, reader)");

    // We check the adapter for monitored fences first, otherwise CreateFence on an adapter without them would
    // print a debug-layer error on every load.
    ComSmartPtr<IDXGIDevice> dxgiDevice;
    ComSmartPtr<IDXGIAdapter> adapter;
    ComSmartPtr<IDXGIAdapter4> adapter4;
    DXGI_ADAPTER_DESC3 adapterDesc{};

    if (FAILED (writer.QueryInterface (dxgiDevice))
        || FAILED (dxgiDevice->GetAdapter (adapter.resetAndGetPointerAddress()))
        || FAILED (adapter.QueryInterface (adapter4))
        || FAILED (adapter4->GetDesc3 (&adapterDesc))
        || (adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SUPPORT_MONITORED_FENCES) == 0)
    {
        return fail (E_NOTIMPL, "adapter without monitored fences");
    }

    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    SharedFenceSync::Parts parts;

    for (int i = 0; i < slotCount; ++i)
    {
        VideoFrameSync::Slot slot;

        if (const auto hr = writerDevice.CreateTexture2D (&desc, nullptr, slot.writerTexture.resetAndGetPointerAddress()); FAILED (hr))
            return fail (hr, "CreateTexture2D (NT shared)");

        ComSmartPtr<IDXGIResource1> resource;

        if (const auto hr = slot.writerTexture.QueryInterface (resource); FAILED (hr))
            return fail (hr, "QueryInterface (IDXGIResource1)");

        HANDLE handle = nullptr;

        if (const auto hr = resource->CreateSharedHandle (nullptr,
                                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                          nullptr,
                                                          &handle);
            FAILED (hr))
        {
            return fail (hr, "IDXGIResource1::CreateSharedHandle");
        }

        const auto openResult = reader5->OpenSharedResource1 (handle, __uuidof (ID3D11Texture2D),
                                                              (void**) slot.readerTexture.resetAndGetPointerAddress());
        CloseHandle (handle);

        if (FAILED (openResult))
            return fail (openResult, "ID3D11Device1::OpenSharedResource1");

        parts.slots.push_back (slot);
    }

    if (const auto hr = writer5->CreateFence (0, D3D11_FENCE_FLAG_SHARED, __uuidof (ID3D11Fence),
                                              (void**) parts.writeFence.resetAndGetPointerAddress());
        FAILED (hr))
    {
        return fail (hr, "ID3D11Device5::CreateFence (shared)");
    }

    HANDLE fenceHandle = nullptr;

    if (const auto hr = parts.writeFence->CreateSharedHandle (nullptr, GENERIC_ALL, nullptr, &fenceHandle); FAILED (hr))
        return fail (hr, "ID3D11Fence::CreateSharedHandle");

    const auto openFenceResult = reader5->OpenSharedFence (fenceHandle, __uuidof (ID3D11Fence),
                                                           (void**) parts.writeFenceOnReader.resetAndGetPointerAddress());
    CloseHandle (fenceHandle);

    if (FAILED (openFenceResult))
        return fail (openFenceResult, "ID3D11Device5::OpenSharedFence");

    if (const auto hr = reader5->CreateFence (0, D3D11_FENCE_FLAG_NONE, __uuidof (ID3D11Fence),
                                              (void**) parts.readFence.resetAndGetPointerAddress());
        FAILED (hr))
    {
        return fail (hr, "ID3D11Device5::CreateFence (reader)");
    }

    ComSmartPtr<ID3D11DeviceContext> writerContext, readerContext;
    writerDevice.GetImmediateContext (writerContext.resetAndGetPointerAddress());
    readerDevice.GetImmediateContext (readerContext.resetAndGetPointerAddress());

    if (const auto hr = writerContext.QueryInterface (parts.writerContext); FAILED (hr))
        return fail (hr, "QueryInterface (ID3D11DeviceContext4, writer)");

    if (const auto hr = readerContext.QueryInterface (parts.readerContext); FAILED (hr))
        return fail (hr, "QueryInterface (ID3D11DeviceContext4, reader)");

    VideoFrameSyncResult result;
    result.sync = std::make_unique<SharedFenceSync> (std::move (parts));
    return result;
}

} // namespace

//==============================================================================
D3D11_TEXTURE2D_DESC makeVideoFrameTextureDesc (int width, int height)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT) width;
    desc.Height = (UINT) height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return desc;
}

VideoFrameSyncResult createVideoFrameSync (VideoFrameSync::Kind kind,
                                           ID3D11Device& writerDevice,
                                           ID3D11Device& readerDevice,
                                           const D3D11_TEXTURE2D_DESC& textureDesc,
                                           int slotCount)
{
    switch (kind)
    {
        case VideoFrameSync::Kind::keyedMutex:   return createKeyedMutexSync (writerDevice, readerDevice, textureDesc, slotCount);
        case VideoFrameSync::Kind::sharedFence:  return createSharedFenceSync (writerDevice, readerDevice, textureDesc, slotCount);
    }

    return fail (E_INVALIDARG, "unknown kind");
}

} // namespace juce::detail
