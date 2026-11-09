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

// We check the HRESULT first, because it names a missing codec, a missing file or a lost device more precisely
// than the MF_MEDIA_ENGINE_ERR_* code.
MediaEngineError mapMediaEngineError (DWORD_PTR mediaErrorCode, HRESULT result)
{
    MediaEngineError error;
    error.result = result;

    if (result == MF_E_TOPO_CODEC_NOT_FOUND)
        error.kind = MediaEngineErrorKind::codecMissing;
    else if (result == HRESULT_FROM_WIN32 (ERROR_FILE_NOT_FOUND) || result == HRESULT_FROM_WIN32 (ERROR_PATH_NOT_FOUND))
        error.kind = MediaEngineErrorKind::sourceNotFound;
    else if (isDeviceLost (result))
        error.kind = MediaEngineErrorKind::deviceLost;
    else if (mediaErrorCode == MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED)
        error.kind = MediaEngineErrorKind::unsupportedSource;
    else if (mediaErrorCode == MF_MEDIA_ENGINE_ERR_DECODE)
        error.kind = MediaEngineErrorKind::decodeFailed;
    else if (mediaErrorCode == MF_MEDIA_ENGINE_ERR_ABORTED)
        error.kind = MediaEngineErrorKind::aborted;
    else
        error.kind = MediaEngineErrorKind::other;

    return error;
}

// Returns 0 for an unknown or infinite duration, which a live stream reports.
double sanitiseDuration (double seconds)
{
    return std::isfinite (seconds) ? seconds : 0.0;
}

// Reads the geometry of the first video stream from the source's native media type, because the engine's size doesn't
// say which rows of a padded stream are visible. Returns nullopt when the file has no such type.
std::optional<VideoFormat> readVideoFormat (const String& path)
{
    ComSmartPtr<IMFSourceReader> reader;
    ComSmartPtr<IMFMediaType> nativeType;
    UINT32 width = 0, height = 0;

    if (FAILED (MFCreateSourceReaderFromURL (path.toWideCharPointer(), nullptr, reader.resetAndGetPointerAddress()))
        || FAILED (reader->GetNativeMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nativeType.resetAndGetPointerAddress()))
        || FAILED (MFGetAttributeSize (nativeType, MF_MT_FRAME_SIZE, &width, &height)))
    {
        return std::nullopt;
    }

    VideoFormat format;
    format.codedSize = { (int) width, (int) height };
    format.visibleArea = format.codedSize;

    MFVideoArea area{};

    if (SUCCEEDED (nativeType->GetBlob (MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*) &area, sizeof (area), nullptr)))
    {
        const Rectangle<int> visible { (int) area.OffsetX.value, (int) area.OffsetY.value, (int) area.Area.cx, (int) area.Area.cy };

        if (! visible.isEmpty() && format.codedSize.contains (visible))
            format.visibleArea = visible;
    }

    UINT32 numerator = 0, denominator = 0;

    if (SUCCEEDED (MFGetAttributeRatio (nativeType, MF_MT_PIXEL_ASPECT_RATIO, &numerator, &denominator))
        && numerator > 0 && denominator > 0)
    {
        format.pixelAspectNum = (int) numerator;
        format.pixelAspectDen = (int) denominator;
    }

    return format;
}

// Returns the file's format when its adjusted size matches the engine's within one pixel. A larger difference means
// the stream changed format during playback, which the file doesn't describe, so we return the whole frame at the
// engine's size.
VideoFormat bindFormat (const std::optional<VideoFormat>& fromFile, Rectangle<int> engineNativeSize)
{
    if (fromFile.has_value())
    {
        const auto adjusted = fromFile->getAdjustedCodedSize();

        if (std::abs (adjusted.getWidth() - engineNativeSize.getWidth()) <= 1 && adjusted.getHeight() == engineNativeSize.getHeight())
            return *fromFile;
    }

    VideoFormat format;
    format.codedSize = engineNativeSize;
    format.visibleArea = engineNativeSize;
    return format;
}

struct SharedState
{
    std::mutex mutex;
    MediaEnginePlayer::Listener* listener = nullptr;
    IMFMediaEngine* engine = nullptr;
    std::optional<VideoFormat> fileFormat;
    VideoFormat format;
};

Rectangle<int> getEngineNativeSize (const SharedState& state)
{
    DWORD width = 0, height = 0;

    if (state.engine == nullptr || FAILED (state.engine->GetNativeVideoSize (&width, &height)))
        return {};

    return { (int) width, (int) height };
}

// Media Foundation threads release this object, sometimes after the player is gone. So we keep our own atomic
// reference count instead of ComBaseClassHelper's. We also reach the listener and the engine only through
// SharedState, which the player clears before it goes.
class NotifyObject : public IMFMediaEngineNotify
{
public:
    NotifyObject (std::shared_ptr<SharedState> stateIn, const String& localPathIn)
        : state (std::move (stateIn)), localPath (localPathIn)
    {
    }

    JUCE_COMRESULT QueryInterface (REFIID refId, void** result) override
    {
        if (result == nullptr)
            return E_POINTER;

        if (refId == __uuidof (IUnknown) || refId == __uuidof (IMFMediaEngineNotify))
        {
            AddRef();
            *result = static_cast<IMFMediaEngineNotify*> (this);
            return S_OK;
        }

        *result = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = --refCount;

        if (remaining == 0)
            delete this;

        return remaining;
    }

    JUCE_COMRESULT EventNotify (DWORD event, DWORD_PTR param1, DWORD param2) override
    {
        // The engine blocks until the Win32 event in param1 is set, so we set it even when the player has already
        // cleared its listener. Otherwise the engine would hang.
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE)
        {
            SetEvent (reinterpret_cast<HANDLE> (param1));
            return S_OK;
        }

        // We read the file before taking SharedState::mutex, so the player's destructor never waits for the read.
        // We read it only at LOADEDMETADATA, because the file's first media type never changes.
        const auto isLoad = event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA;
        const auto loadedFormat = isLoad && localPath.isNotEmpty() ? readVideoFormat (localPath) : std::nullopt;

        const std::scoped_lock lock { state->mutex };

        if (state->listener == nullptr)
            return S_OK;

        if (isLoad)
            state->fileFormat = loadedFormat;

        if (isLoad || event == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE)
            state->format = bindFormat (state->fileFormat, getEngineNativeSize (*state));

        switch (event)
        {
            case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:  state->listener->loaded (state->format); break;
            case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:    state->listener->formatChanged (state->format); break;
            case MF_MEDIA_ENGINE_EVENT_ENDED:           state->listener->ended(); break;

            // We pass on every one of these events, because the listener re-reads its state after each one.
            // Playback that a load starts is reported only through these events.
            case MF_MEDIA_ENGINE_EVENT_PLAY:
            case MF_MEDIA_ENGINE_EVENT_PLAYING:
            case MF_MEDIA_ENGINE_EVENT_PAUSE:
            case MF_MEDIA_ENGINE_EVENT_SEEKED:
                state->listener->stateChanged();
                break;

            case MF_MEDIA_ENGINE_EVENT_ERROR:
            {
                auto result = (HRESULT) param2;
                ComSmartPtr<IMFMediaError> mediaError;

                if (state->engine != nullptr && SUCCEEDED (state->engine->GetError (mediaError.resetAndGetPointerAddress()))
                    && mediaError != nullptr)
                {
                    if (const auto extendedResult = mediaError->GetExtendedErrorCode(); FAILED (extendedResult))
                        result = extendedResult;
                }

                state->listener->errorOccurred (mapMediaEngineError (param1, result));
                break;
            }

            default:
                break;
        }

        return S_OK;
    }

private:
    std::shared_ptr<SharedState> state;
    const String localPath;
    std::atomic<ULONG> refCount { 1 };
};

} // namespace

//==============================================================================
HRESULT startMediaFoundation()
{
    static const HRESULT result = MFStartup (MF_VERSION);
    return result;
}

//==============================================================================
VideoDevice createVideoDevice (IDXGIAdapter* adapter)
{
    const auto driverType = adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL preferredLevels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    VideoDevice video;
    video.result = D3D11CreateDevice (adapter, driverType, nullptr, flags, preferredLevels, (UINT) std::size (preferredLevels),
                                      D3D11_SDK_VERSION, video.device.resetAndGetPointerAddress(), nullptr, nullptr);

    // An adapter without feature level 11_0 fails the call above, so we retry with the default feature levels
    // before giving up on the adapter.
    if (FAILED (video.result))
        video.result = D3D11CreateDevice (adapter, driverType, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                          video.device.resetAndGetPointerAddress(), nullptr, nullptr);

    if (FAILED (video.result))
    {
        video.device = nullptr;
        return video;
    }

    // Media Foundation threads call the device while we transfer frames on the message thread,
    // so we turn on the device's multithread protection.
    ComSmartPtr<ID3D10Multithread> multithread;

    if (const auto hr = video.device.QueryInterface (multithread); FAILED (hr))
    {
        video.device = nullptr;
        video.result = hr;
        return video;
    }

    multithread->SetMultithreadProtected (TRUE);

    ComSmartPtr<IDXGIDevice> dxgiDevice;
    ComSmartPtr<IDXGIAdapter> deviceAdapter;
    DXGI_ADAPTER_DESC description{};

    if (SUCCEEDED (video.device.QueryInterface (dxgiDevice))
        && SUCCEEDED (dxgiDevice->GetAdapter (deviceAdapter.resetAndGetPointerAddress()))
        && SUCCEEDED (deviceAdapter->GetDesc (&description)))
    {
        video.adapterLuid = description.AdapterLuid;
    }

    return video;
}

//==============================================================================
String describeMediaEngineError (const MediaEngineError& error)
{
    const auto text = [&]() -> String
    {
        switch (error.kind)
        {
            case MediaEngineErrorKind::codecMissing:              return "A codec needed to play this video is not installed";
            case MediaEngineErrorKind::unsupportedSource:         return "The video format is not supported";
            case MediaEngineErrorKind::sourceNotFound:            return "The video file was not found";
            case MediaEngineErrorKind::decodeFailed:              return "The video could not be decoded";
            case MediaEngineErrorKind::deviceLost:                return "The graphics device was lost";
            case MediaEngineErrorKind::aborted:                   return "Loading the video was aborted";
            case MediaEngineErrorKind::audioEndpointUnsupported:
                return "Selecting the audio output device is not supported on this version of Windows";
            case MediaEngineErrorKind::platformUnavailable:       return "Media Foundation could not be started";
            case MediaEngineErrorKind::other:                     return "Media Foundation error";
        }

        return "Media Foundation error";
    }();

    return text + " (" + getHresultText (error.result) + ")";
}

bool isDeviceLost (HRESULT result)
{
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG;
}

String getHresultText (HRESULT result)
{
    return "0x" + String::toHexString ((int64) (uint32) result).paddedLeft ('0', 8);
}

//==============================================================================
Rectangle<int> VideoFormat::getDisplaySize() const
{
    return { roundToInt ((double) visibleArea.getWidth() * pixelAspectNum / pixelAspectDen), visibleArea.getHeight() };
}

Rectangle<int> VideoFormat::getAdjustedCodedSize() const
{
    return { roundToInt ((double) codedSize.getWidth() * pixelAspectNum / pixelAspectDen), codedSize.getHeight() };
}

std::optional<MFVideoNormalizedRect> VideoFormat::getSourceRect() const
{
    if (visibleArea == codedSize || codedSize.isEmpty())
        return std::nullopt;

    return MFVideoNormalizedRect { (float) visibleArea.getX()      / (float) codedSize.getWidth(),
                                   (float) visibleArea.getY()      / (float) codedSize.getHeight(),
                                   (float) visibleArea.getRight()  / (float) codedSize.getWidth(),
                                   (float) visibleArea.getBottom() / (float) codedSize.getHeight() };
}

//==============================================================================
class MediaEnginePlayer::Impl
{
public:
    Impl() = default;

    ~Impl()
    {
        {
            const std::scoped_lock lock { state->mutex };
            state->listener = nullptr;
            state->engine = nullptr;
        }

        if (engine != nullptr)
            engine->Shutdown();
    }

    std::shared_ptr<SharedState> state = std::make_shared<SharedState>();
    ComSmartPtr<IMFDXGIDeviceManager> deviceManager;
    ComSmartPtr<NotifyObject> notify;
    ComSmartPtr<IMFMediaEngine> engine;
};

MediaEnginePlayer::MediaEnginePlayer (std::unique_ptr<Impl> implIn)
    : impl (std::move (implIn))
{
}

MediaEnginePlayer::~MediaEnginePlayer() = default;

std::variant<std::unique_ptr<MediaEnginePlayer>, MediaEngineError> MediaEnginePlayer::open (ID3D11Device& device,
                                                                                           Listener& listener,
                                                                                           const URL& source,
                                                                                           const String& audioEndpointId)
{
    if (const auto hr = startMediaFoundation(); FAILED (hr))
        return MediaEngineError { MediaEngineErrorKind::platformUnavailable, hr };

    const auto fail = [] (HRESULT hr) { return mapMediaEngineError (0, hr); };

    const auto localPath = source.isLocalFile() ? source.getLocalFile().getFullPathName() : String();
    const auto sourceText = source.isLocalFile() ? localPath : source.toString (true);

    auto impl = std::make_unique<Impl>();

    UINT resetToken = 0;

    if (const auto hr = MFCreateDXGIDeviceManager (&resetToken, impl->deviceManager.resetAndGetPointerAddress()); FAILED (hr))
        return fail (hr);

    if (const auto hr = impl->deviceManager->ResetDevice (&device, resetToken); FAILED (hr))
        return fail (hr);

    impl->notify = ComSmartPtr<NotifyObject> { new NotifyObject (impl->state, localPath), IncrementRef::no };

    ComSmartPtr<IMFAttributes> attributes;

    if (const auto hr = MFCreateAttributes (attributes.resetAndGetPointerAddress(), 3); FAILED (hr))
        return fail (hr);

    if (const auto hr = attributes->SetUnknown (MF_MEDIA_ENGINE_CALLBACK, impl->notify.get()); FAILED (hr))
        return fail (hr);

    if (const auto hr = attributes->SetUnknown (MF_MEDIA_ENGINE_DXGI_MANAGER, impl->deviceManager.get()); FAILED (hr))
        return fail (hr);

    // We don't set MF_MEDIA_ENGINE_PLAYBACK_HWND, so the engine runs in frame-server mode: it draws nothing
    // itself and writes a frame in this format when we ask for one.
    if (const auto hr = attributes->SetUINT32 (MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM); FAILED (hr))
        return fail (hr);

    ComSmartPtr<IMFMediaEngineClassFactory> factory;

    if (const auto hr = factory.CoCreateInstance (CLSID_MFMediaEngineClassFactory); FAILED (hr))
        return fail (hr);

    if (const auto hr = factory->CreateInstance (0, attributes.get(), impl->engine.resetAndGetPointerAddress()); FAILED (hr))
        return fail (hr);

    // We set the endpoint before SetSource(), so that the source opens its audio on that endpoint.
    if (audioEndpointId.isNotEmpty())
    {
        ComSmartPtr<IMFMediaEngineAudioEndpointId> endpointSetter;

        if (const auto hr = impl->engine.QueryInterface (endpointSetter); FAILED (hr))
            return MediaEngineError { MediaEngineErrorKind::audioEndpointUnsupported, hr };

        if (const auto hr = endpointSetter->SetAudioEndpointId (audioEndpointId.toWideCharPointer()); FAILED (hr))
            return fail (hr);
    }

    {
        const std::scoped_lock lock { impl->state->mutex };
        impl->state->listener = &listener;
        impl->state->engine = impl->engine.get();
    }

    auto* sourceString = SysAllocString (sourceText.toWideCharPointer());

    if (sourceString == nullptr)
        return fail (E_OUTOFMEMORY);

    const auto setSourceResult = impl->engine->SetSource (sourceString);
    SysFreeString (sourceString);

    if (FAILED (setSourceResult))
        return fail (setSourceResult);

    return std::make_unique<MediaEnginePlayer> (std::move (impl));
}

void MediaEnginePlayer::play()                          { impl->engine->Play(); }
void MediaEnginePlayer::pause()                         { impl->engine->Pause(); }
void MediaEnginePlayer::seek (double seconds)           { impl->engine->SetCurrentTime (seconds); }

void MediaEnginePlayer::setPlaybackRate (double rate)
{
    // We set the default rate too, because the engine may return to it after a seek or at the end of the stream.
    impl->engine->SetDefaultPlaybackRate (rate);
    impl->engine->SetPlaybackRate (rate);
}

double MediaEnginePlayer::getPlaybackRate() const       { return impl->engine->GetPlaybackRate(); }
void MediaEnginePlayer::setVolume (double newVolume)    { impl->engine->SetVolume (newVolume); }
double MediaEnginePlayer::getPosition() const           { return impl->engine->GetCurrentTime(); }
double MediaEnginePlayer::getDuration() const           { return sanitiseDuration (impl->engine->GetDuration()); }
bool MediaEnginePlayer::isEnded() const                 { return impl->engine->IsEnded() != FALSE; }
bool MediaEnginePlayer::isSeeking() const               { return impl->engine->IsSeeking() != FALSE; }

MediaEnginePlayer::Tick MediaEnginePlayer::tick()
{
    LONGLONG presentationTime = 0;
    const auto result = impl->engine->OnVideoStreamTick (&presentationTime);

    if (result == S_OK)
        return { TickStatus::newFrame, (double) presentationTime / 10000000.0, result };

    // S_FALSE means that no new frame is due. We treat a failure that is not a device loss the same way,
    // because the next call will try again.
    return { isDeviceLost (result) ? TickStatus::deviceLost : TickStatus::noNewFrame, 0.0, result };
}

MediaEnginePlayer::Transfer MediaEnginePlayer::transfer (ID3D11Texture2D& target, const RECT& destination)
{
    const auto sourceRect = [this]
    {
        const std::scoped_lock lock { impl->state->mutex };
        return impl->state->format.getSourceRect();
    }();

    const MFARGB border { 0, 0, 0, 255 };
    const auto result = impl->engine->TransferVideoFrame (&target,
                                                          sourceRect.has_value() ? &*sourceRect : nullptr,
                                                          &destination,
                                                          &border);

    if (SUCCEEDED (result))
        return { FrameStatus::transferred, result };

    return { isDeviceLost (result) ? FrameStatus::deviceLost : FrameStatus::failed, result };
}

} // namespace juce::detail
