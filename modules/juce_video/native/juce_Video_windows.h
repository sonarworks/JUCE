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

/*  The Windows VideoComponent::Pimpl plays the video with the Media Foundation Media Engine in frame-server mode. A
    window drawn with Direct2D on the engine's adapter draws each frame without a copy. Otherwise, or when the shared
    ring of textures can't be created, frames go through main memory.

    Everything runs on the message thread except EventSink's Listener methods. Every owner callback is posted through
    notify(), so a handler may delete the VideoComponent or load another video.
*/
struct VideoComponent::Pimpl : public Component
{
    Pimpl (VideoComponent& ownerIn, bool /*useNativeControlsIfAvailable*/)
        : owner (ownerIn)
    {
        setOpaque (true);
    }

    //==============================================================================
    Result load (const File& file)
    {
        auto result = loadSync (URL (file));

        if (result.wasOk())
            currentFile = file;

        return result;
    }

    Result load (const URL& url)
    {
        auto result = loadSync (url);

        if (result.wasOk())
            currentURL = url;

        return result;
    }

    void loadAsync (const URL& url, std::function<void (const URL&, Result)> callback)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        close();
        pendingLoad = { url, std::move (callback) };
        phase = Phase::loading;

        if (auto result = openSource (url, EventSink::Mode::postEvents); result.failed())
            fail (result.getErrorMessage());
    }

    void close()
    {
        JUCE_ASSERT_MESSAGE_THREAD

        ++closeCount;

        // We post a failure to a loadAsync() callback that hasn't been called yet, so that its caller learns that
        // the load was cancelled.
        if (auto pending = std::exchange (pendingLoad, {}); pending.callback != nullptr)
            notify ([pending] (Pimpl&) { pending.callback (pending.url, Result::fail ("Loading cancelled")); });

        closeEngine();
        currentFile = File();
        currentURL = URL();
        timeline = {};
        transport = Transport::stopped;
        presenter.reset();
        repaint();
        owner.resized();
        publish();
    }

    bool isOpen() const                 { return phase == Phase::open || phase == Phase::reopening; }
    Rectangle<int> getNativeSize() const { return isOpen() ? videoSize : Rectangle<int>(); }
    double getDuration() const          { return phase == Phase::open ? player->getDuration() : timeline.duration; }
    bool isEnded() const                { return transport == Transport::ended; }

    // A video that hasn't opened yet doesn't play, even if the owner asked for it.
    bool isPlaying() const              { return isOpen() && transport == Transport::playing; }

    void play()
    {
        JUCE_ASSERT_MESSAGE_THREAD

        // The engine's end event may still be on its way, so we check the engine too. We seek to 0 ourselves, because
        // a reopened engine at the end doesn't report the end and wouldn't go back to 0 by itself.
        if (phase == Phase::open && player->isEnded())
            applyEnd();

        if (transport == Transport::ended)
            setPosition (0.0);

        transport = Transport::playing;

        if (phase == Phase::open)
            player->play();

        if (isOpen())
            publish();
    }

    void stop()
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (transport == Transport::playing)
            transport = Transport::stopped;

        if (phase == Phase::open)
            player->pause();

        if (isOpen())
            publish();
    }

    void setPosition (double seconds)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (phase == Phase::open)
        {
            const auto clockBeforeSeek = player->getPosition();
            const auto queuedWithNewTarget = player->isSeeking() && std::abs (seconds - pendingSeek.value_or (clockBeforeSeek)) >= 0.001;
            seekEngine (seconds);

            // A seek that doesn't move the clock gives no new frame, so we don't await a landing for it, otherwise a new
            // FramePath would stay black. A seek queued behind another one moves the clock only later, so for it we
            // compare the target instead of the clock.
            if (queuedWithNewTarget || std::abs (player->getPosition() - clockBeforeSeek) >= 0.001)
                presenter.awaitLanding();
        }
        else
        {
            presenter.awaitLanding();
            timeline.position = seconds;
        }

        if (transport == Transport::ended)
            transport = Transport::stopped;
    }

    // While our seek is in progress we return its target, because the clock doesn't move for a seek queued behind
    // another one. A reopen restores the position we return, so returning the clock would lose that seek.
    // We limit the position to the duration, because the clock runs past it at the end.
    double getPosition() const
    {
        if (phase != Phase::open)
            return timeline.position;

        const auto clock = player->getPosition();
        const auto position = player->isSeeking() ? pendingSeek.value_or (clock) : clock;
        const auto duration = player->getDuration();
        return duration > 0.0 ? jmin (position, duration) : position;
    }

    void setSpeed (double newSpeed)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        playbackRate = newSpeed;

        if (phase == Phase::open)
            player->setPlaybackRate (newSpeed);
    }

    double getSpeed() const             { return phase == Phase::open ? player->getPlaybackRate() : playbackRate; }

    void setVolume (float newVolume)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        volume = jlimit (0.0f, 1.0f, newVolume);

        if (phase == Phase::open)
            player->setVolume (volume);
    }

    float getVolume() const             { return volume; }

    void setAudioOutputDevice (const String& deviceID)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (deviceID == audioEndpointId)
            return;

        audioEndpointId = deviceID;

        if (player != nullptr)
            reopen (ReopenDevice::sameDevice);
    }

    String getAudioOutputDevice() const { return audioEndpointId; }

    //==============================================================================
    void paint (Graphics& g) override
    {
        if (player == nullptr)
        {
            g.fillAll (Colours::grey);
            return;
        }

        g.fillAll (Colours::black);

        if (framePath == nullptr)
            return;

        if (const auto image = framePath->getImage(); image.isValid())
        {
            g.setImageResamplingQuality (Graphics::mediumResamplingQuality);
            g.drawImage (image, getLocalBounds().toFloat());
        }
    }

    File currentFile;
    URL currentURL;

private:
    //==============================================================================
    using FrameStatus = detail::MediaEnginePlayer::FrameStatus;
    using TickStatus = detail::MediaEnginePlayer::TickStatus;
    using Transfer = detail::MediaEnginePlayer::Transfer;

    enum class Phase
    {
        closed,      // no engine
        loading,     // a new source is opening, and load() or the loadAsync() callback waits for the result
        open,        // playing or paused
        reopening    // the current source is opened again, and the position and duration come from timeline
    };

    enum class ReopenDevice
    {
        sameDevice,
        onWindowAdapter
    };

    enum class Transport
    {
        stopped,
        playing,
        ended    // stopped at the end of the file, where the next play() starts again from 0
    };

    //==============================================================================
    // Holds the lock of JUCE's multithreaded Direct2D factory. Direct2D requires it around any outside call on
    // JUCE's immediate Direct3D context, because the context isn't thread-safe.
    struct ScopedDirect2DLock
    {
        ScopedDirect2DLock (ID2D1Multithread& multithreadIn) : multithread (multithreadIn) { multithread.Enter(); }
        ~ScopedDirect2DLock() { multithread.Leave(); }

        ID2D1Multithread& multithread;

        JUCE_DECLARE_NON_COPYABLE (ScopedDirect2DLock)
    };

    struct FramePath
    {
        struct FrameResult
        {
            Transfer transfer;
            bool imageChanged = false;
        };

        virtual ~FramePath() = default;

        // Finishes the work of an earlier writeFrame(). The caller should call this on every vertical blank while the
        // component is showing.
        virtual FrameResult finishPending() = 0;

        // Writes the engine's current frame for getImage(). Returns noNewFrame when the path has no room yet, and the
        // caller should try again on the next vertical blank.
        virtual FrameResult writeFrame (detail::MediaEnginePlayer& player) = 0;

        // Takes the engine's current frame without showing it, unless the path has no free texture for it.
        virtual Transfer discardFrame (detail::MediaEnginePlayer& player) = 0;

        // Returns whether the path shows a frame or is about to, so that the presenter can accept an older frame for a
        // path that lost its picture.
        virtual bool hasFrame() const = 0;

        virtual Image getImage() const = 0;
    };

    //==============================================================================
    // Frames stay on the GPU: the engine writes a ring of textures that JUCE's device draws.
    class GpuFramePath : public FramePath
    {
    public:
        GpuFramePath() = default;   // use create()

        static std::unique_ptr<GpuFramePath> create (ID3D11Device& engineDevice,
                                                     const DxgiAdapter& adapter,
                                                     Rectangle<int> videoSize,
                                                     String& failure)
        {
            auto path = std::make_unique<GpuFramePath>();

            ComSmartPtr<ID2D1Factory> factory;
            adapter.direct2DDevice->GetFactory (factory.resetAndGetPointerAddress());

            if (const auto hr = factory.QueryInterface (path->multithread); FAILED (hr))
            {
                failure = "ID2D1Multithread " + detail::getHresultText (hr);
                return nullptr;
            }

            // The texture holds only the video frame at the video's size, and Direct2D scales it when painting,
            // so we don't recreate the ring on a resize of the component.
            const auto desc = detail::makeVideoFrameTextureDesc (videoSize.getWidth(), videoSize.getHeight());

            // We prefer the shared fence, because the keyed mutex can make the reader miss a frame. Where the fence is
            // refused we fall back to the keyed mutex, which still beats a copy through main memory.
            auto created = detail::createVideoFrameSync (detail::VideoFrameSync::Kind::sharedFence,
                                                         engineDevice, *adapter.direct3DDevice, desc, ringSize);

            if (created.sync == nullptr)
                created = detail::createVideoFrameSync (detail::VideoFrameSync::Kind::keyedMutex,
                                                        engineDevice, *adapter.direct3DDevice, desc, ringSize);

            if (created.sync == nullptr)
            {
                failure = String (created.failedStep) + " " + detail::getHresultText (created.result);
                return nullptr;
            }

            path->sync = std::move (created.sync);
            path->destination = { 0, 0, (LONG) desc.Width, (LONG) desc.Height };

            const auto bitmapContext = Direct2DDeviceContext::create (adapter.direct2DDevice);

            if (bitmapContext == nullptr)
            {
                failure = "ID2D1DeviceContext";
                return nullptr;
            }

            // The engine may write alpha 0 in a letterbox border even when asked for 255, so we create the bitmaps
            // with alpha ignored.
            const auto bitmapProperties = D2D1::BitmapProperties1 (D2D1_BITMAP_OPTIONS_NONE,
                                                                   D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                                      D2D1_ALPHA_MODE_IGNORE));

            for (int i = 0; i < path->sync->getSlotCount(); ++i)
            {
                ComSmartPtr<IDXGISurface> surface;
                ComSmartPtr<ID2D1Bitmap1> bitmap;

                if (const auto hr = path->sync->getSlot (i).readerTexture.QueryInterface (surface); FAILED (hr))
                {
                    failure = "IDXGISurface " + detail::getHresultText (hr);
                    return nullptr;
                }

                if (const auto hr = bitmapContext->CreateBitmapFromDxgiSurface (surface, &bitmapProperties,
                                                                                bitmap.resetAndGetPointerAddress());
                    FAILED (hr))
                {
                    failure = "CreateBitmapFromDxgiSurface " + detail::getHresultText (hr);
                    return nullptr;
                }

                path->images.push_back (detail::createDirect2DExternalImage (adapter.direct2DDevice, bitmap));
            }

            return path;
        }

        ~GpuFramePath() override
        {
            // The keyed-mutex ring gives back its key on JUCE's immediate context.
            if (multithread != nullptr)
            {
                const ScopedDirect2DLock lock (*multithread);
                sync = nullptr;
            }
        }

        FrameResult finishPending() override
        {
            FrameResult result;

            if (const auto slot = sync->getPending(); slot >= 0)
                result.imageChanged = display (slot);

            return result;
        }

        FrameResult writeFrame (detail::MediaEnginePlayer& player) override
        {
            FrameResult result;
            const auto slot = sync->beginWrite();

            if (slot < 0)
                return result;

            result.transfer = player.transfer (*sync->getSlot (slot).writerTexture, destination);

            if (result.transfer.status != FrameStatus::transferred)
            {
                sync->cancelWrite (slot);
                return result;
            }

            sync->endWrite (slot);
            result.imageChanged = display (slot);
            return result;
        }

        Transfer discardFrame (detail::MediaEnginePlayer& player) override
        {
            const auto slot = sync->beginWrite();

            if (slot < 0)
                return {};

            const auto transfer = player.transfer (*sync->getSlot (slot).writerTexture, destination);
            sync->cancelWrite (slot);
            return transfer;
        }

        bool hasFrame() const override      { return displayedImage.isValid() || sync->getPending() >= 0; }
        Image getImage() const override     { return displayedImage; }

    private:
        static constexpr int ringSize = 3;

        // Returns whether the reader took the slot. A slot it couldn't take stays pending for finishPending().
        bool display (int slot)
        {
            const ScopedDirect2DLock lock (*multithread);

            if (! sync->setDisplayed (slot))
                return false;

            displayedImage = images[(size_t) slot];
            return true;
        }

        ComSmartPtr<ID2D1Multithread> multithread;
        std::unique_ptr<detail::VideoFrameSync> sync;
        std::vector<Image> images;
        Image displayedImage;
        RECT destination{};
    };

    //==============================================================================
    // Copies each frame through main memory into a native Image, for a window that the GPU path can't serve.
    class CpuFramePath : public FramePath
    {
    public:
        CpuFramePath() = default;   // use create()

        static std::unique_ptr<CpuFramePath> create (ID3D11Device& engineDevice, Rectangle<int> videoSize, String& failure)
        {
            auto path = std::make_unique<CpuFramePath>();
            auto desc = detail::makeVideoFrameTextureDesc (videoSize.getWidth(), videoSize.getHeight());

            if (const auto hr = engineDevice.CreateTexture2D (&desc, nullptr, path->target.resetAndGetPointerAddress()); FAILED (hr))
            {
                failure = "CreateTexture2D " + detail::getHresultText (hr);
                return nullptr;
            }

            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            if (const auto hr = engineDevice.CreateTexture2D (&desc, nullptr, path->staging.resetAndGetPointerAddress()); FAILED (hr))
            {
                failure = "CreateTexture2D (staging) " + detail::getHresultText (hr);
                return nullptr;
            }

            engineDevice.GetImmediateContext (path->context.resetAndGetPointerAddress());
            path->destination = { 0, 0, (LONG) desc.Width, (LONG) desc.Height };

            path->image = Image (Image::ARGB, videoSize.getWidth(), videoSize.getHeight(), true, NativeImageType());
            return path;
        }

        // Reads the pending copy if the GPU has finished it, and keeps it pending while the GPU still copies.
        FrameResult finishPending() override
        {
            FrameResult result;

            if (! stagingPending)
                return result;

            const auto readResult = readStaging();

            if (readResult == S_OK)
            {
                result.imageChanged = true;
            }
            else if (readResult != DXGI_ERROR_WAS_STILL_DRAWING)
            {
                // We report a failed Map() like a failed transfer, so a lost device reaches fail(). We also clear
                // stagingPending, otherwise no new copy could start.
                stagingPending = false;
                result.transfer = { detail::isDeviceLost (readResult) ? FrameStatus::deviceLost : FrameStatus::failed, readResult };
            }

            return result;
        }

        FrameResult writeFrame (detail::MediaEnginePlayer& player) override
        {
            FrameResult result;

            // We wait for the pending copy to be read, because with a new copy in the staging texture Map() would keep
            // reporting that the GPU is still copying, and a copy slower than the frame period would never be read.
            if (stagingPending)
                return result;

            result.transfer = player.transfer (*target, destination);

            if (result.transfer.status == FrameStatus::transferred)
            {
                context->CopyResource (staging, target);

                // We submit the copy at once, because a paused video may give the engine device no other work, and
                // the copy could then stay unsubmitted.
                context->Flush();
                stagingPending = true;
            }

            return result;
        }

        Transfer discardFrame (detail::MediaEnginePlayer& player) override
        {
            return player.transfer (*target, destination);
        }

        bool hasFrame() const override      { return imageHasFrame || stagingPending; }
        Image getImage() const override     { return imageHasFrame ? image : Image(); }

    private:
        // Returns DXGI_ERROR_WAS_STILL_DRAWING while the GPU still copies.
        HRESULT readStaging()
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};

            // We don't let Map() wait for the GPU to finish the copy, so the message thread doesn't block on it.
            if (const auto hr = context->Map (staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped); FAILED (hr))
                return hr;

            {
                const Image::BitmapData bitmapData (image, Image::BitmapData::writeOnly);
                const auto rowBytes = (size_t) image.getWidth() * 4;

                for (int row = 0; row < image.getHeight(); ++row)
                    std::memcpy (bitmapData.getLinePointer (row),
                                 static_cast<const uint8*> (mapped.pData) + (size_t) row * mapped.RowPitch,
                                 rowBytes);
            }

            context->Unmap (staging, 0);
            stagingPending = false;
            imageHasFrame = true;
            return S_OK;
        }

        ComSmartPtr<ID3D11DeviceContext> context;
        ComSmartPtr<ID3D11Texture2D> target;
        ComSmartPtr<ID3D11Texture2D> staging;
        bool stagingPending = false;
        RECT destination{};
        Image image;
        bool imageHasFrame = false;
    };

    //==============================================================================
    using LoadOutcome = std::variant<detail::VideoFormat, detail::MediaEngineError>;

    // Receives the engine's events on Media Foundation threads. A synchronous load takes the first outcome directly,
    // and the rest is posted to the message thread. Each player gets its own sink, so we drop an old player's posted
    // events by destroying its sink.
    class EventSink : public detail::MediaEnginePlayer::Listener
    {
    public:
        enum class Mode
        {
            postEvents,
            captureLoadOutcome
        };

        EventSink (Pimpl& pimplIn, Mode mode)
            : pimpl (pimplIn), waitsForLoad (mode == Mode::captureLoadOutcome)
        {
        }

        // Returns nullopt when neither the load nor an error arrived in time.
        std::optional<LoadOutcome> waitForLoad (int timeoutMs)
        {
            loadFinished.wait (timeoutMs);

            const std::scoped_lock lock { mutex };
            waitsForLoad = false;
            return std::exchange (loadOutcome, std::nullopt);
        }

        void loaded (const detail::VideoFormat& format) override
        {
            if (takeLoadOutcome (format))
                return;

            post ([format] (Pimpl& p) { p.handleLoaded (format); });
        }

        void formatChanged (const detail::VideoFormat& format) override
        {
            post ([format] (Pimpl& p) { p.handleFormatChanged (format); });
        }

        void stateChanged() override    { post ([] (Pimpl& p) { p.handleStateChanged(); }); }
        void ended() override           { post ([] (Pimpl& p) { p.handleEnded(); }); }

        void errorOccurred (detail::MediaEngineError error) override
        {
            if (takeLoadOutcome (error))
                return;

            post ([error] (Pimpl& p) { p.handleError (error); });
        }

    private:
        bool takeLoadOutcome (LoadOutcome outcome)
        {
            const std::scoped_lock lock { mutex };

            if (! waitsForLoad || loadOutcome.has_value())
                return false;

            loadOutcome = std::move (outcome);
            loadFinished.signal();
            return true;
        }

        template <typename Function>
        void post (Function function)
        {
            MessageManager::callAsync ([&target = pimpl, weakAlive = std::weak_ptr<bool> (alive), function]
            {
                // The sink is destroyed only on the message thread, so if alive still exists here, the Pimpl does too.
                if (weakAlive.lock() != nullptr)
                    function (target);
            });
        }

        Pimpl& pimpl;
        const std::shared_ptr<bool> alive = std::make_shared<bool> (true);

        std::mutex mutex;
        bool waitsForLoad = false;
        std::optional<LoadOutcome> loadOutcome;
        WaitableEvent loadFinished;
    };

    struct PendingLoad
    {
        URL url;
        std::function<void (const URL&, Result)> callback;
    };

    //==============================================================================
    Result loadSync (const URL& url)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        close();
        phase = Phase::loading;

        if (auto result = openSource (url, EventSink::Mode::captureLoadOutcome); result.failed())
        {
            closeEngine();
            return result;
        }

        // The engine raises its events on Media Foundation threads, so this wait needs no message loop.
        const auto outcome = eventSink->waitForLoad (url.isLocalFile() ? 10000 : 30000);

        if (! outcome.has_value())
        {
            closeEngine();
            return Result::fail ("Timed out while loading the video");
        }

        if (const auto* error = std::get_if<detail::MediaEngineError> (&*outcome))
        {
            closeEngine();
            return Result::fail (detail::describeMediaEngineError (*error));
        }

        if (auto result = finishLoad (std::get<detail::VideoFormat> (*outcome)); result.failed())
        {
            closeEngine();
            return result;
        }

        return Result::ok();
    }

    // Opens the source on the current audio output. The callers set the phase.
    Result openSource (const URL& url, EventSink::Mode mode)
    {
        if (auto result = ensureEngineDevice(); result.failed())
            return result;

        // We drop the old player before its sink, because a player calls its sink until the player is destroyed.
        player = nullptr;
        eventSink = std::make_unique<EventSink> (*this, mode);
        auto opened = detail::MediaEnginePlayer::open (*engineDevice, *eventSink, url, audioEndpointId);

        if (const auto* error = std::get_if<detail::MediaEngineError> (&opened))
        {
            eventSink = nullptr;
            return Result::fail (detail::describeMediaEngineError (*error));
        }

        player = std::move (std::get<std::unique_ptr<detail::MediaEnginePlayer>> (opened));
        openedSource = url;
        return Result::ok();
    }

    Result ensureEngineDevice()
    {
        if (engineDevice != nullptr)
            return Result::ok();

        // An owner may load from a listener that JUCE calls before deviceAttachment learns the window's adapter, so we
        // read the adapter here. No player exists here, so the onChange that update() may call does nothing.
        deviceAttachment.update();
        const auto adapter = deviceAttachment.getAdapter();
        auto video = detail::createVideoDevice (adapter != nullptr ? adapter->dxgiAdapter.get() : nullptr);

        // We fall back to the default adapter. If it isn't the window's adapter, frames reach the window
        // through main memory.
        if (video.device == nullptr && adapter != nullptr)
            video = detail::createVideoDevice (nullptr);

        if (video.device == nullptr)
            return Result::fail ("Failed to create a Direct3D 11 device with video support ("
                                 + detail::getHresultText (video.result) + ")");

        engineDevice = video.device;
        engineAdapterLuid = video.adapterLuid;
        return Result::ok();
    }

    // Moves a loading or reopening video to Phase::open. The engine's own events after play() report the playback
    // that finishLoad() starts, so finishLoad() doesn't call publish().
    Result finishLoad (const detail::VideoFormat& format)
    {
        const auto newVideoSize = format.getDisplaySize();

        if (newVideoSize != videoSize)
            framePath = nullptr;

        videoSize = newVideoSize;

        if (auto result = buildFramePath(); result.failed())
            return result;

        phase = Phase::open;

        if (timeline.position > 0.0)
            seekEngine (timeline.position);

        if (playbackRate != 1.0)
            player->setPlaybackRate (playbackRate);

        player->setVolume (volume);

        if (transport == Transport::playing)
            player->play();

        owner.resized();
        return Result::ok();
    }

    // Drops the engine and its device, and keeps the transport, the timeline and the audio output.
    void closeEngine()
    {
        player = nullptr;
        eventSink = nullptr;
        framePath = nullptr;
        engineDevice = nullptr;
        phase = Phase::closed;
        videoSize = {};
        pendingSeek.reset();
        transferFailuresSinceImageChange = 0;
        lastFailedFrame.reset();
    }

    // The only caller of the engine's seek. The engine doesn't pause at the end of the file, and a seek would then
    // resume playback, so we apply the end first even if its posted event hasn't arrived yet.
    void seekEngine (double seconds)
    {
        if (player->isEnded())
            applyEnd();

        pendingSeek = seconds;
        player->seek (seconds);
    }

    // The end of the file stops the video until the next play().
    void applyEnd()
    {
        transport = Transport::ended;
        player->pause();
        publish();
    }

    // Opens the source again at the current position and play state. The last frame stays on screen unless the
    // engine moves to the window's adapter or a format change during playback made the size differ from the file's.
    void reopen (ReopenDevice device)
    {
        if (phase == Phase::open)
        {
            // The old player's posted events are dropped with its sink, so we apply its end and report its last
            // state ourselves.
            if (player->isEnded())
                applyEnd();

            timeline = { getPosition(), player->getDuration() };
            phase = Phase::reopening;
            publish();
        }

        player = nullptr;
        pendingSeek.reset();
        presenter.engineReplaced();
        transferFailuresSinceImageChange = 0;
        lastFailedFrame.reset();

        if (device == ReopenDevice::onWindowAdapter)
        {
            framePath = nullptr;
            engineDevice = nullptr;
        }

        if (auto result = openSource (openedSource, EventSink::Mode::postEvents); result.failed())
            fail (result.getErrorMessage());
    }

    // Closes the video and reports the failure to a waiting loadAsync() callback, otherwise to onErrorOccurred unless
    // another close() comes first.
    void fail (const String& message)
    {
        // We check pendingLoad, not the phase, because the phase is already open while an ok result waits for
        // delivery.
        auto pending = std::exchange (pendingLoad, {});
        jassert (pending.callback != nullptr || phase != Phase::closed);

        close();

        if (pending.callback != nullptr)
        {
            notify ([pending, message] (Pimpl&) { pending.callback (pending.url, Result::fail (message)); });
            return;
        }

        notify ([message, postedCloseCount = closeCount] (Pimpl& p)
        {
            // The onPlaybackStopped handler may have loaded another video, so we report the error only if no close()
            // came after this error was posted.
            if (p.closeCount != postedCloseCount)
                return;

            const auto callback = p.owner.onErrorOccurred;
            NullCheckedInvocation::invoke (callback, message);
        });
    }

    // The only path to the owner's callbacks. We post them, so a handler may delete the VideoComponent or load another
    // video. A posted call should copy the owner's callback before calling it, otherwise a handler that deletes the
    // VideoComponent destroys the callback while it runs.
    void notify (std::function<void (Pimpl&)> call)
    {
        MessageManager::callAsync ([safeThis = SafePointer<Pimpl> (this), call = std::move (call)]
        {
            if (safeThis != nullptr)
                call (*safeThis);
        });
    }

    // Tells the owner what changed: playing or stopped, then the end of the file. Each posted call reads the state when
    // it runs and reports only a change since the last report, so a callback never describes a state that is gone.
    void publish()
    {
        notify ([] (Pimpl& p) { p.reportPlaying(); });
        notify ([] (Pimpl& p) { p.reportEnded(); });
    }

    void reportPlaying()
    {
        const auto playing = isPlaying();

        if (playing == reported.playing)
            return;

        reported.playing = playing;
        const auto callback = playing ? owner.onPlaybackStarted : owner.onPlaybackStopped;
        NullCheckedInvocation::invoke (callback);
    }

    void reportEnded()
    {
        const auto ended = isEnded();

        if (ended == reported.ended)
            return;

        reported.ended = ended;

        if (ended)
        {
            const auto callback = owner.onEndReached;
            NullCheckedInvocation::invoke (callback);
        }
    }

    //==============================================================================
    void handleLoaded (const detail::VideoFormat& format)
    {
        if (phase != Phase::loading && phase != Phase::reopening)
            return;

        // We post the loadAsync() callback before finishLoad() starts playback, so it reaches the owner before any
        // callback that the started playback raises.
        if (pendingLoad.callback != nullptr)
        {
            currentURL = pendingLoad.url;

            // The loadAsync() callback stays in pendingLoad until it runs, so close() or fail() can still fail it.
            // A reopen posts this lambda again, and that second run finds pendingLoad empty and does nothing.
            notify ([postedCloseCount = closeCount] (Pimpl& p)
            {
                if (p.closeCount != postedCloseCount || p.pendingLoad.callback == nullptr)
                    return;

                auto pending = std::exchange (p.pendingLoad, {});
                pending.callback (pending.url, Result::ok());
            });
        }

        if (auto result = finishLoad (format); result.failed())
            fail (result.getErrorMessage());
    }

    void handleStateChanged()
    {
        if (phase != Phase::open)
            return;

        publish();
    }

    // Applies the end of the file unless the engine has already left it, because the engine's event may be older than
    // a seek or a play() of the owner.
    void handleEnded()
    {
        if (phase == Phase::open && player->isEnded())
            applyEnd();
    }

    // Drops framePath when the stream's frame size changes during playback, so that the next vblank builds a
    // FramePath of the new size.
    void handleFormatChanged (const detail::VideoFormat& format)
    {
        const auto newVideoSize = format.getDisplaySize();

        if (phase != Phase::open || newVideoSize.isEmpty() || newVideoSize == videoSize)
            return;

        framePath = nullptr;
        videoSize = newVideoSize;
        owner.resized();
        repaint();
    }

    // We end the video on an engine error, also when the engine's device is lost, because we leave a recovery to the
    // owner, which can load the video again.
    void handleError (detail::MediaEngineError error)
    {
        if (player != nullptr)
            fail (detail::describeMediaEngineError (error));
    }

    static String describeDeviceLoss (HRESULT result)
    {
        return detail::describeMediaEngineError ({ detail::MediaEngineErrorKind::deviceLost, result });
    }

    //==============================================================================
    bool isOnEngineAdapter (const DxgiAdapter& adapter) const
    {
        const auto luid = adapter.getAdapterUniqueID();
        return luid.LowPart == engineAdapterLuid.LowPart && luid.HighPart == engineAdapterLuid.HighPart;
    }

    Result buildFramePath()
    {
        if (framePath != nullptr || videoSize.isEmpty())
            return Result::ok();

        const auto adapter = deviceAttachment.getAdapter();

        if (adapter != nullptr && isOnEngineAdapter (*adapter))
        {
            String failure;
            framePath = GpuFramePath::create (*engineDevice, *adapter, videoSize, failure);

            if (framePath == nullptr)
            {
                JUCE_VIDEO_LOG ("Video frames are copied through main memory, because the GPU ring failed: " + failure);
            }
        }

        if (framePath == nullptr)
        {
            String failure;
            framePath = CpuFramePath::create (*engineDevice, videoSize, failure);

            if (framePath == nullptr)
                return Result::fail ("Failed to create the video frame buffers: " + failure);
        }

        return Result::ok();
    }

    // Writes the engine's current frame if the presenter allows it. When the path has no room yet, the frame is tried
    // again on the next vblank, because a paused video has no newer frame to come.
    Transfer writeCurrentFrame()
    {
        const auto frame = presenter.getFrameToWrite (framePath->hasFrame());

        if (! frame.has_value())
            return {};

        const auto written = framePath->writeFrame (*player);

        if (written.imageChanged)
            repaint();

        recordTransfer (written.transfer, frame, written.imageChanged);

        if (written.transfer.status == FrameStatus::transferred)
            presenter.shown (*frame);

        return written.transfer;
    }

    // We count a failed transfer once per frame, otherwise one bad frame retried on every vblank would soon reach the
    // limit and close the video. A failed read of a copy has no frame, so it counts every time. Only a changed image
    // resets the count, so a copy whose read always fails still reaches the limit.
    void recordTransfer (const Transfer& transfer, std::optional<double> frame, bool imageChanged)
    {
        if (imageChanged)
        {
            transferFailuresSinceImageChange = 0;
            lastFailedFrame.reset();
        }
        else if (transfer.status == FrameStatus::failed && (! frame.has_value() || frame != lastFailedFrame))
        {
            ++transferFailuresSinceImageChange;
            lastFailedFrame = frame;
            JUCE_VIDEO_LOG ("Video frame transfer failed: " + detail::getHresultText (transfer.result));
        }
    }

    // Reopens the engine when the window is drawn on another adapter. Otherwise we let the next vblank rebuild
    // framePath, because a removed adapter leaves getAdapter() empty until then.
    void handleWindowDeviceChange()
    {
        if (player == nullptr)
            return;

        if (const auto adapter = deviceAttachment.getAdapter(); adapter != nullptr && ! isOnEngineAdapter (*adapter))
        {
            // Not tested: cross-GPU window transfer handling. Reached by a transparent window on a monitor of another
            // GPU, or after an adapter was created again with a new LUID.
            reopen (ReopenDevice::onWindowAdapter);
            return;
        }

        framePath = nullptr;
        repaint();
    }

    void onVBlank()
    {
        if (phase != Phase::open)
            return;

        // We tick a hidden video too, because the engine doesn't finish a seek without ticks. We read the seek state
        // before the tick, otherwise a seek ending in between would land a frame from before the seek.
        const auto engineSeeking = player->isSeeking();
        const auto tick = player->tick();

        if (! engineSeeking)
            pendingSeek.reset();

        if (tick.status == TickStatus::deviceLost)
        {
            fail (describeDeviceLoss (tick.result));
            return;
        }

        if (tick.status == TickStatus::newFrame)
            presenter.frameTicked (tick.presentationTime, engineSeeking);

        if (! isShowing())
            return;

        if (framePath == nullptr)
        {
            if (auto result = buildFramePath(); result.failed())
            {
                fail (result.getErrorMessage());
                return;
            }

            if (framePath == nullptr)
                return;
        }

        const auto finished = framePath->finishPending();

        if (finished.imageChanged)
            repaint();

        recordTransfer (finished.transfer, std::nullopt, finished.imageChanged);

        auto transfer = finished.transfer.status == FrameStatus::noNewFrame ? writeCurrentFrame() : finished.transfer;

        // A reopened engine can stall for about a second while nobody takes a frame it handed out, so we take a refused
        // frame. We don't during a landing, otherwise a paused video could lose the only frame its seek gets.
        if (tick.status == TickStatus::newFrame && transfer.status == FrameStatus::noNewFrame && ! presenter.isAwaitingLanding()
            && ! presenter.getFrameToWrite (framePath->hasFrame()).has_value())
        {
            transfer = framePath->discardFrame (*player);
            recordTransfer (transfer, tick.presentationTime, false /* imageChanged */);
        }

        if (transfer.status == FrameStatus::deviceLost)
            fail (describeDeviceLoss (transfer.result));
        else if (transfer.status == FrameStatus::failed && transferFailuresSinceImageChange >= maxTransferFailures)
            fail (detail::describeMediaEngineError ({ detail::MediaEngineErrorKind::other, transfer.result }));
    }

    //==============================================================================
    VideoComponent& owner;

    // The owner's settings, kept across a reopen and a load and given to every new engine.
    String audioEndpointId;
    double playbackRate = 1.0;
    float volume = 1.0f;

    // The owner's transport state, which isPlaying() and isEnded() read, because the engine loses its state in a reopen
    // and doesn't pause at the end of the file.
    Transport transport = Transport::stopped;

    // The position and duration that getPosition() and getDuration() read outside Phase::open.
    struct Timeline
    {
        double position = 0.0;
        double duration = 0.0;
    };

    Timeline timeline;
    std::optional<double> pendingSeek;   // the target of our last seek, until a vblank sees the engine not seeking
    detail::VideoFramePresenter presenter;

    Phase phase = Phase::closed;
    URL openedSource;
    PendingLoad pendingLoad;
    Rectangle<int> videoSize;

    // What reportPlaying() and reportEnded() last reported, so that an unchanged state isn't reported again.
    struct Reported
    {
        bool playing = false;
        bool ended = false;
    };

    Reported reported;

    // Increased by every close(). The posted onErrorOccurred call and the posted ok result of loadAsync() run only if
    // closeCount is unchanged, so neither runs after a later close() or load.
    int closeCount = 0;

    // Frame transfers that keep failing would leave the picture frozen without an error, so we close the video
    // after this many frames fail with no change of the image.
    static constexpr int maxTransferFailures = 30;
    int transferFailuresSinceImageChange = 0;
    std::optional<double> lastFailedFrame;

    ComSmartPtr<ID3D11Device> engineDevice;
    LUID engineAdapterLuid{};

    // We declare these before the player, so the ring is released after the player that writes into it,
    // and the sink after the player that calls it.
    std::unique_ptr<FramePath> framePath;
    std::unique_ptr<EventSink> eventSink;
    std::unique_ptr<detail::MediaEnginePlayer> player;

    // Declared last, so no callback of theirs runs while the members above are destroyed.
    detail::Direct2DDeviceAttachment deviceAttachment { *this, [this] { handleWindowDeviceChange(); } };
    VBlankAttachment vBlankAttachment { this, [this] { onVBlank(); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pimpl)
};
