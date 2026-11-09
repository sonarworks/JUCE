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

/*  Returns an Image over a Direct2D bitmap that the caller owns and writes outside JUCE, for example a video frame.

    A Direct2D context on the given device draws the bitmap without a copy. Any other reader gets a copy in main
    memory, and black pixels if the copy fails.

    The caller must finish each write before JUCE reads the bitmap, for example with a keyed mutex or a fence.

    The Image is read-only: a writable Image::BitmapData gets a cleared throwaway image.

    The bitmap must be DXGI_FORMAT_B8G8R8A8_UNORM, with D2D1_ALPHA_MODE_IGNORE (gives Image::RGB) or
    D2D1_ALPHA_MODE_PREMULTIPLIED (gives Image::ARGB). Any other format gives a null Image.
*/
Image createDirect2DExternalImage (ComSmartPtr<ID2D1Device1> device, ComSmartPtr<ID2D1Bitmap1> bitmap);

} // namespace juce::detail

#endif
