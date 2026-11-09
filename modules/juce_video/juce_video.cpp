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

#ifdef JUCE_VIDEO_H_INCLUDED
 /* When you add this cpp file to your project, you mustn't include it in a file where you've
    already included any other headers - just put it inside a file on its own, possibly with your config
    flags preceding it, but don't include anything else. That also includes avoiding any automatic prefix
    header files that the compiler may be using.
 */
 #error "Incorrect use of JUCE cpp file"
#endif

#define JUCE_CORE_INCLUDE_OBJC_HELPERS 1
#define JUCE_CORE_INCLUDE_JNI_HELPERS 1
#define JUCE_CORE_INCLUDE_COM_SMART_PTR 1
#define JUCE_CORE_INCLUDE_NATIVE_HEADERS 1

#include "juce_video.h"

#if JUCE_MAC || JUCE_IOS
 #import <AVFoundation/AVFoundation.h>
 #import <AVKit/AVKit.h>

//==============================================================================
#elif JUCE_WINDOWS
 #include <d2d1_3.h>
 #include <d3d11_4.h>
 #include <dcomp.h>
 #include <dwrite_3.h>
 #include <dxgi1_6.h>
 #include <mfapi.h>
 #include <mferror.h>
 #include <mfmediaengine.h>
 #include <mfreadwrite.h>

 #if JUCE_USE_CAMERA
  #include "dshow.h"
  #include "dshowasf.h"
  #include "strmif.h"
  #include "wmsdk.h"
 #endif

 #if ! JUCE_DONT_AUTOLINK_TO_WIN32_LIBRARIES
  #pragma comment (lib, "d3d11.lib")
  #pragma comment (lib, "mfplat.lib")
  #pragma comment (lib, "mfreadwrite.lib")
  #pragma comment (lib, "mfuuid.lib")

  #if JUCE_USE_CAMERA
   #pragma comment (lib, "Strmiids.lib")
   #pragma comment (lib, "wmvcore.lib")
  #endif
 #endif

 #include <juce_graphics/fonts/juce_FunctionPointerDestructor.h>
 #include <juce_graphics/native/juce_Direct2DMetrics_windows.h>
 #include <juce_graphics/native/juce_Direct2DGraphicsContext_windows.h>
 #include <juce_graphics/native/juce_DirectX_windows.h>
 #include <juce_graphics/native/juce_Direct2DPixelDataPage_windows.h>
 #include <juce_graphics/images/juce_ImagePixelDataNativeExtensions.h>
 #include <juce_graphics/native/juce_Direct2DExternalImage_windows.h>
 #include <juce_gui_basics/native/juce_Direct2DDeviceAttachment_windows.h>

 #include "native/juce_MediaFoundation_windows.h"
 #include "native/juce_VideoFrameSync_windows.h"
 #include "native/juce_VideoFramePresenter_windows.h"
#endif

//==============================================================================
#include "playback/juce_VideoComponent.cpp"

#if JUCE_WINDOWS
 #include "native/juce_MediaFoundation_windows.cpp"
 #include "native/juce_VideoFrameSync_windows.cpp"
#endif

#if JUCE_USE_CAMERA
 #include "capture/juce_CameraDevice.cpp"
#endif
