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

class Direct2DExternalImageType : public ImageType
{
public:
    // This type can't make an image without an outside bitmap, so create() returns an ordinary native image.
    ImagePixelData::Ptr create (Image::PixelFormat format, int w, int h, bool shouldClearImage) const override
    {
        return NativeImageType{}.create (format, w, h, shouldClearImage);
    }

    // A distinct id makes JUCE convert this image before drawing it on another device. With the native id JUCE would
    // skip the conversion, and that device would draw nothing.
    int getTypeID() const override
    {
        return ByteOrder::makeInt ('d', '2', 'x', 'b');
    }
};

//==============================================================================
class Direct2DExternalPixelData : public ImagePixelData
{
public:
    Direct2DExternalPixelData (ComSmartPtr<ID2D1Device1> deviceIn, ComSmartPtr<ID2D1Bitmap1> bitmapIn, Image::PixelFormat formatIn)
        : ImagePixelData (formatIn, (int) bitmapIn->GetPixelSize().width, (int) bitmapIn->GetPixelSize().height),
          device (deviceIn),
          page { bitmapIn, {} }
    {
    }

    std::unique_ptr<LowLevelGraphicsContext> createLowLevelContext() override
    {
        // Drawing into the bitmap would race its outside writer, so a Graphics on this image draws into a throwaway
        // image.
        return SoftwareImageType{}.create (pixelFormat, width, height, false)->createLowLevelContext();
    }

    Ptr clone() override
    {
        return readBack();
    }

    std::unique_ptr<ImageType> createType() const override
    {
        return std::make_unique<Direct2DExternalImageType>();
    }

    void initialiseBitmapData (Image::BitmapData& bitmapData, int x, int y, Image::BitmapData::ReadWriteMode mode) override
    {
        // Writes can't reach the bitmap, so a writable BitmapData gets a cleared throwaway image.
        const auto copy = mode == Image::BitmapData::readOnly ? readBack()
                                                              : SoftwareImageType{}.create (pixelFormat, width, height, true);
        copy->initialiseBitmapData (bitmapData, x, y, mode);
        bitmapData.dataReleaser = std::make_unique<CopyKeeper> (copy, std::move (bitmapData.dataReleaser));
    }

    NativeExtensions getNativeExtensions() override
    {
        struct Wrapped
        {
            Wrapped (Direct2DExternalPixelData* selfIn) : self (selfIn) {}

            Span<const Direct2DPixelDataPage> getPages (ComSmartPtr<ID2D1Device1> x) const
            {
                if (x.get() != self->device.get())
                    return {};

                return { &self->page, 1 };
            }

            Point<int> getTopLeft() const
            {
                return {};
            }

            Direct2DExternalPixelData* self = nullptr;
        };

        return NativeExtensions { Wrapped { this } };
    }

private:
    struct CopyKeeper : public Image::BitmapData::BitmapDataReleaser
    {
        CopyKeeper (ImagePixelData::Ptr copyIn, std::unique_ptr<BitmapDataReleaser> innerIn)
            : copy (std::move (copyIn)), inner (std::move (innerIn)) {}

        ImagePixelData::Ptr copy;
        std::unique_ptr<BitmapDataReleaser> inner;
    };

    ImagePixelData::Ptr readBack() const
    {
        const auto argb = SoftwareImageType{}.create (Image::ARGB, width, height, false);
        Image argbImage { argb };

        if (! copyBitmapInto (argbImage))
            argbImage.clear ({ width, height }, Colours::black);

        if (pixelFormat == Image::ARGB)
            return argb;

        return argbImage.convertedToFormat (pixelFormat).getPixelData();
    }

    bool copyBitmapInto (Image& target) const
    {
        const auto context = Direct2DDeviceContext::create (device);

        if (context == nullptr)
            return false;

        const auto size = page.bitmap->GetPixelSize();
        const auto properties = D2D1::BitmapProperties1 (D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         page.bitmap->GetPixelFormat());

        ComSmartPtr<ID2D1Bitmap1> readable;

        if (FAILED (context->CreateBitmap (size, nullptr, 0, properties, readable.resetAndGetPointerAddress())))
            return false;

        const auto destinationPoint = D2D1::Point2U();
        const auto sourceRect = D2D1::RectU (0, 0, size.width, size.height);

        if (FAILED (readable->CopyFromBitmap (&destinationPoint, page.bitmap, &sourceRect)))
            return false;

        D2D1_MAPPED_RECT mapped{};

        if (FAILED (readable->Map (D2D1_MAP_OPTIONS_READ, &mapped)))
            return false;

        const ScopeGuard unmap { [&] { readable->Unmap(); } };

        const Image::BitmapData destination { target, Image::BitmapData::writeOnly };

        // With D2D1_ALPHA_MODE_IGNORE the alpha bytes are undefined, so we force them opaque for an RGB image.
        const auto forceOpaque = pixelFormat == Image::RGB;

        for (int row = 0; row < height; ++row)
        {
            auto* line = destination.getLinePointer (row);
            std::memcpy (line, mapped.bits + (size_t) row * mapped.pitch, (size_t) width * 4);

            if (forceOpaque)
                for (int column = 0; column < width; ++column)
                    line[column * 4 + 3] = 0xff;
        }

        return true;
    }

    ComSmartPtr<ID2D1Device1> device;
    Direct2DPixelDataPage page;
};

//==============================================================================
Image createDirect2DExternalImage (ComSmartPtr<ID2D1Device1> device, ComSmartPtr<ID2D1Bitmap1> bitmap)
{
    if (device == nullptr || bitmap == nullptr)
        return {};

    const auto format = bitmap->GetPixelFormat();

    if (format.format != DXGI_FORMAT_B8G8R8A8_UNORM)
        return {};

    if (format.alphaMode != D2D1_ALPHA_MODE_IGNORE && format.alphaMode != D2D1_ALPHA_MODE_PREMULTIPLIED)
        return {};

    const auto pixelFormat = format.alphaMode == D2D1_ALPHA_MODE_IGNORE ? Image::RGB : Image::ARGB;

    return Image { new Direct2DExternalPixelData { device, bitmap, pixelFormat } };
}

} // namespace juce::detail
