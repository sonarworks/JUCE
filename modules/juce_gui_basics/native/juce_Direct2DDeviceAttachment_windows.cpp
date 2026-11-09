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

Direct2DDeviceAttachment::Direct2DDeviceAttachment (Component& componentIn, std::function<void()> onChangeIn)
    : component (componentIn),
      onChange (std::move (onChangeIn))
{
    JUCE_ASSERT_MESSAGE_THREAD

    directX->adapters.addListener (*this);
    component.addComponentListener (this);

    if (auto* peer = dynamic_cast<HWNDComponentPeer*> (component.getPeer()))
        adapter = peer->getRenderAdapter();
}

Direct2DDeviceAttachment::~Direct2DDeviceAttachment()
{
    component.removeComponentListener (this);
    directX->adapters.removeListener (*this);
}

void Direct2DDeviceAttachment::update()
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto* peer = dynamic_cast<HWNDComponentPeer*> (component.getPeer());
    auto newAdapter = peer != nullptr ? peer->getRenderAdapter() : nullptr;

    if (newAdapter == adapter)
        return;

    adapter = std::move (newAdapter);
    NullCheckedInvocation::invoke (onChange);
}

// We check here too, so that a Component that gets a window learns its adapter before the first vblank of that window.
void Direct2DDeviceAttachment::componentParentHierarchyChanged (Component&)
{
    update();
}

void Direct2DDeviceAttachment::adapterRemoved (DxgiAdapter::Ptr expiringAdapter)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // The swap-chain renderer may draw with a new device before the next vblank, so we report the change in this call,
    // while resources on the old device can still be released.
    if (adapter == nullptr || adapter != expiringAdapter)
        return;

    adapter = nullptr;
    NullCheckedInvocation::invoke (onChange);
}

} // namespace juce::detail
