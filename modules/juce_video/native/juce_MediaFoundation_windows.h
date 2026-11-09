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

/*  Starts Media Foundation on the first call and returns the result of MFStartup(). We never call MFShutdown(),
    because it dispatches window messages while it waits, and a paint there can read a component being destroyed.
*/
HRESULT startMediaFoundation();

//==============================================================================
struct VideoDevice
{
    ComSmartPtr<ID3D11Device> device;   // null on failure
    LUID adapterLuid {};
    HRESULT result = E_FAIL;
};

/*  Creates a Direct3D 11 device the Media Engine can decode and convert on. A null adapter means the
    default hardware adapter.
*/
VideoDevice createVideoDevice (IDXGIAdapter* adapter);

//==============================================================================
enum class MediaEngineErrorKind
{
    codecMissing,
    unsupportedSource,
    sourceNotFound,
    decodeFailed,
    deviceLost,
    aborted,
    audioEndpointUnsupported,
    platformUnavailable,
    other
};

struct MediaEngineError
{
    MediaEngineErrorKind kind = MediaEngineErrorKind::other;
    HRESULT result = S_OK;
};

/*  A sentence for VideoComponent::onErrorOccurred and load() results, ending with the HRESULT in hex. */
String describeMediaEngineError (const MediaEngineError& error);

bool isDeviceLost (HRESULT result);

String getHresultText (HRESULT result);   // "0x8007000e"

//==============================================================================
/*  The geometry of a video stream. The engine's size includes the rows that some encoders pad, so we take the visible
    area from a local file whose size matches the engine's.
*/
struct VideoFormat
{
    Rectangle<int> codedSize;     // the frame with its padded rows, or the engine's size when the file doesn't match
    Rectangle<int> visibleArea;   // inside codedSize
    int pixelAspectNum = 1, pixelAspectDen = 1;

    // The visible area adjusted by the pixel aspect ratio, the native size of the video.
    Rectangle<int> getDisplaySize() const;

    // The coded size adjusted by the pixel aspect ratio, which should match the engine's GetNativeVideoSize() within
    // one pixel of rounding.
    Rectangle<int> getAdjustedCodedSize() const;

    // The visible area as a part of the coded frame, empty when the whole frame is visible.
    std::optional<MFVideoNormalizedRect> getSourceRect() const;
};

//==============================================================================
/*  Plays one source with the Media Foundation Media Engine in frame-server mode, with its audio on one render endpoint.

    Every method must be called on the thread that called open(). Listener calls arrive on Media Foundation threads
    and can start before open() returns.
*/
class MediaEnginePlayer
{
public:
    enum class FrameStatus
    {
        noNewFrame,
        transferred,
        deviceLost,
        failed
    };

    struct Transfer
    {
        FrameStatus status = FrameStatus::noNewFrame;
        HRESULT result = S_OK;
    };

    enum class TickStatus
    {
        noNewFrame,
        newFrame,
        deviceLost
    };

    struct Tick
    {
        TickStatus status = TickStatus::noNewFrame;
        double presentationTime = 0.0;   // seconds, of a new frame
        HRESULT result = S_OK;
    };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void loaded (const VideoFormat& format) = 0;
        virtual void formatChanged (const VideoFormat& format) = 0;
        // Called after the engine's play, pause and seek events.
        virtual void stateChanged() = 0;
        virtual void ended() = 0;
        virtual void errorOccurred (MediaEngineError error) = 0;
    };

    /*  An empty audioEndpointId means the system default output. The device must come from createVideoDevice(), and
        the listener must outlive the player. A source that isn't a local file shows a padded stream's padded rows.
    */
    static std::variant<std::unique_ptr<MediaEnginePlayer>, MediaEngineError> open (ID3D11Device& device,
                                                                                    Listener& listener,
                                                                                    const URL& source,
                                                                                    const String& audioEndpointId);

    ~MediaEnginePlayer();   // no Listener call starts or runs after it returns

    void play();
    void pause();
    void seek (double seconds);
    void setPlaybackRate (double rate);
    double getPlaybackRate() const;
    void setVolume (double newVolume);   // 0..1, the engine's own volume, not the system's

    double getPosition() const;   // seconds
    double getDuration() const;   // seconds, 0 before Listener::loaded()
    bool isEnded() const;
    bool isSeeking() const;

    /*  Asks the engine for a new frame. The caller should do this on every vertical blank while the video is open,
        whether or not it shows the frames, because the engine doesn't finish a seek otherwise.
    */
    Tick tick();

    /*  Writes the engine's current frame into the target, scaled to the destination rectangle. The target must be a
        B8G8R8A8_UNORM render-target texture on the player's device.
    */
    Transfer transfer (ID3D11Texture2D& target, const RECT& destination);

private:
    class Impl;

public:
    MediaEnginePlayer (std::unique_ptr<Impl> implIn);   // used by open() through std::make_unique

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (MediaEnginePlayer)
};

} // namespace juce::detail

#endif
