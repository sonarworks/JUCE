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

/*  Tracks the DXGI adapter whose Direct2D device draws the window of a Component, so that GPU
    resources JUCE draws without a copy can be created on that device.

    Message thread only. The Component must outlive this object.
*/
class Direct2DDeviceAttachment : private DxgiAdapterListener,
                                 private ComponentListener
{
public:
    /*  onChange is called when getAdapter() changes. Release resources made on the old adapter's devices in that call,
        because JUCE may already draw with the new device and would read their images through main memory or draw black.
    */
    Direct2DDeviceAttachment (Component& component, std::function<void()> onChange);
    ~Direct2DDeviceAttachment() override;

    /*  Returns nullptr when the Component has no window, isn't drawn with Direct2D, or lost its adapter before the next
        vblank. A transparent window can report a removed adapter until its next frame, so handle failures on it.
    */
    DxgiAdapter::Ptr getAdapter() const { return adapter; }

    /*  Reads the window's adapter at once and calls onChange if it changed. Call it before getAdapter() in a hierarchy
        callback that JUCE may run before this object's own one, such as a listener of a parent Component, because
        getAdapter() could still return the previous adapter or nullptr there.
    */
    void update();

private:
    void componentParentHierarchyChanged (Component&) override;
    void adapterCreated (DxgiAdapter::Ptr) override {}
    void adapterRemoved (DxgiAdapter::Ptr expiringAdapter) override;

    Component& component;
    std::function<void()> onChange;
    SharedResourcePointer<DirectX> directX;
    DxgiAdapter::Ptr adapter;

    // A renderer or transparency switch raises no event of its own, so we check the window on every vblank.
    VBlankAttachment vBlankAttachment { &component, [this] { update(); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Direct2DDeviceAttachment)
};

} // namespace juce::detail

#endif
