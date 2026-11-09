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

/*  Decides whether the Media Engine's current frame may reach the screen. After awaitLanding() nothing is written until
    the landing, the first frame ticked after the engine's seek ended. Otherwise the picture moves only forward, except
    within keptPositionTolerance on a new FramePath and after an unwritten landing.
*/
class VideoFramePresenter
{
public:
    // Forgets everything, for a new source.
    void reset()                        { *this = {}; }

    // Call this for a seek after which the picture may go back.
    void awaitLanding()                 { awaitingLanding = true; }

    void engineReplaced()               { currentFrame.reset(); }
    void shown (double seconds)         { lastShown = seconds; }
    bool isAwaitingLanding() const      { return awaitingLanding; }

    // We decide when a seek has landed on a tick, not on a write, because a hidden video writes no frames.
    // A frame ticked while the engine still seeks may be from before the seek, so it doesn't count as the landing.
    // The engine hands out the seek's frame again on a later tick.
    void frameTicked (double seconds, bool engineSeeking)
    {
        currentFrame = seconds;

        if (awaitingLanding && ! engineSeeking)
        {
            awaitingLanding = false;
            landingFloor = seconds;
            lastShown.reset();
        }
    }

    // Returns the time of the engine's current frame if that frame should be written.
    std::optional<double> getFrameToWrite (bool pathHasFrame) const
    {
        if (awaitingLanding || ! currentFrame.has_value())
            return std::nullopt;

        // A reopen can happen before the landing frame is written. We then refuse the new engine's frames that are far
        // behind the landing frame, otherwise the new engine's first frames would flash on screen.
        if (! lastShown.has_value())
            return *currentFrame >= landingFloor - keptPositionTolerance ? currentFrame : std::nullopt;

        // A paused video gets no newer frame after its FramePath is rebuilt, so we accept a frame close to the one
        // the old FramePath showed.
        const auto allowed = *currentFrame > *lastShown || (! pathHasFrame && *currentFrame >= *lastShown - keptPositionTolerance);

        return allowed ? currentFrame : std::nullopt;
    }

private:
    // A frame's time can run two frames ahead of the clock, so after a reopen the engine may restore a frame older than
    // the last one shown. We accept a frame this much older on a new FramePath and after a landing that wasn't written.
    // Otherwise a paused video would keep an old picture or stay black.
    static constexpr double keptPositionTolerance = 0.25;   // seconds

    std::optional<double> currentFrame;
    std::optional<double> lastShown;
    double landingFloor = 0.0;
    bool awaitingLanding = false;
};

} // namespace juce::detail

#endif
