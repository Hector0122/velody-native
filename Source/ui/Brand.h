#pragma once
#include <JuceHeader.h>
#include "../Theme.h"

// Velody's mark (Resources/brand/velody-icon.svg): a blue→violet rounded square with five stems whose
// ends draw a V. Used for the window icon, the header and the .deb's icon (written from here so they match).
namespace Brand
{
    inline std::unique_ptr<Drawable> logo()
    {
        return Drawable::createFromImageData (BinaryData::velodyicon_svg, BinaryData::velodyicon_svgSize);
    }

    inline Image icon (int size)
    {
        Image img (Image::ARGB, size, size, true);
        Graphics g (img);
        if (auto d = logo())
            d->drawWithin (g, Rectangle<float> ((float) size, (float) size), RectanglePlacement::centred, 1.0f);
        return img;
    }

    // Header: the mark and "Velody" in a blue→violet gradient.
    class Wordmark : public Component
    {
    public:
        Wordmark() : mark (logo()) { setInterceptsMouseClicks (false, false); }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            auto m = r.removeFromLeft (r.getHeight()).reduced (2);
            if (mark != nullptr)
                mark->drawWithin (g, m, RectanglePlacement::centred, 1.0f);
            r.removeFromLeft (10);
            const auto font = Theme::title (jmin (24.0f, r.getHeight() * 0.7f));
            g.setFont (font);
            g.setGradientFill (ColourGradient (Theme::primary.brighter (0.2f), r.getX(), 0, Theme::accent.brighter (0.2f),
                                               r.getX() + GlyphArrangement::getStringWidth (font, "Velody"), 0, false));
            g.drawText ("Velody", r, Justification::centredLeft);
        }

    private:
        std::unique_ptr<Drawable> mark;
    };
}
