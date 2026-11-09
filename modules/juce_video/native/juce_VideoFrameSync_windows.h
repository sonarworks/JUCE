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

#if JUCE_WINDOWS

namespace juce::detail
{

/*  A ring of textures that a writer device, the Media Engine's, fills and a reader device, JUCE's, draws on the same
    adapter, with the reader's GPU work ordered after the writer's.

    Make every call on one thread. Hold the reader context's lock, ID2D1Multithread for JUCE's device, around
    setDisplayed() and the destructor, and make the writer device multithread-protected when other threads use it.
*/
class VideoFrameSync
{
public:
    enum class Kind
    {
        keyedMutex,    // legacy shared handles with IDXGIKeyedMutex
        sharedFence    // NT handles with a shared ID3D11Fence, adapters with monitored fences
    };

    struct Slot
    {
        ComSmartPtr<ID3D11Texture2D> writerTexture;
        ComSmartPtr<ID3D11Texture2D> readerTexture;
    };

    virtual ~VideoFrameSync() = default;

    virtual int getSlotCount() const = 0;
    virtual const Slot& getSlot (int index) const = 0;

    /*  Returns a slot that is not displayed, not pending and not read by the GPU, or -1 when every slot is busy. End
        each returned slot with endWrite() or cancelWrite() before the next call, otherwise a keyed-mutex ring keeps it.
    */
    virtual int beginWrite() = 0;

    /*  Call this after the writer's commands for the slot are queued. The reader may use the slot after
        setDisplayed().
    */
    virtual void endWrite (int slot) = 0;

    /*  Gives back a slot from beginWrite() whose content nobody should read. */
    virtual void cancelWrite (int slot) = 0;

    /*  From now on the reader's GPU work waits for the slot's last write. Returns false when the reader can't take the
        slot yet: the slot stays pending, and the caller should retry, because a paused video may write no newer frame.
    */
    virtual bool setDisplayed (int slot) = 0;

    /*  Returns the slot that the reader couldn't take yet, -1 when there is none. */
    virtual int getPending() const = 0;
};

/*  A texture description for video frames of the given size. A ring adds its sharing flags to it. */
D3D11_TEXTURE2D_DESC makeVideoFrameTextureDesc (int width, int height);

struct VideoFrameSyncResult
{
    std::unique_ptr<VideoFrameSync> sync;   // null on failure
    HRESULT result = S_OK;
    const char* failedStep = "";
};

/*  Creates slotCount textures described by textureDesc. The kind sets their MiscFlags. */
VideoFrameSyncResult createVideoFrameSync (VideoFrameSync::Kind kind,
                                           ID3D11Device& writerDevice,
                                           ID3D11Device& readerDevice,
                                           const D3D11_TEXTURE2D_DESC& textureDesc,
                                           int slotCount);

} // namespace juce::detail

#endif
