#pragma once
#include <JuceHeader.h>
#include "BinaryData.h"

// Velody's look, from the Electron app's brand kit (assets/main.css): near-black neutrals, blue primary
// (#3b82f6), violet accent (#7b6bf5), Archivo Black for titles and IBM Plex Mono for numbers.
namespace Theme
{
    inline const Colour bg       { 0xff0a0a0b }; // window
    inline const Colour panel    { 0xff141416 }; // header, transport, mixer
    inline const Colour raised   { 0xff1c1c20 }; // buttons, boxes, track headers
    inline const Colour lane     { 0xff111113 }; // timeline lanes
    inline const Colour outline  { 0xff2a2a30 };
    inline const Colour text     { 0xffe5e5e5 };
    inline const Colour muted    { 0xff8a8a93 };
    inline const Colour primary  { 0xff3b82f6 };
    inline const Colour accent   { 0xff7b6bf5 };
    inline const Colour record   { 0xffef4444 };
    inline const Colour mute     { 0xfff59e0b };
    inline const Colour solo     { 0xff3b82f6 };
    inline const Colour success  { 0xff22c55e };

    constexpr float radius = 8.0f;

    // One colour per track (header stripe, clips, waveform, mixer strip), cycling.
    inline Colour trackColour (int index)
    {
        static const Colour palette[] { Colour (0xff3b82f6), Colour (0xff7b6bf5), Colour (0xffec4899), Colour (0xfff59e0b),
                                        Colour (0xff10b981), Colour (0xff06b6d4), Colour (0xfff97316), Colour (0xff84cc16) };
        return palette[(size_t) (jmax (0, index) % 8)];
    }

    // Ruler/grid spacing: the first of these steps (seconds) at least `minPixels` apart.
    inline double gridStep (double pixelsPerSecond, double minPixels = 80.0)
    {
        for (double step : { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 })
            if (step * pixelsPerSecond >= minPixels)
                return step;
        return 600.0;
    }

    inline Typeface::Ptr titleTypeface()
    {
        static auto t = Typeface::createSystemTypefaceFor (BinaryData::ArchivoBlackRegular_ttf, BinaryData::ArchivoBlackRegular_ttfSize);
        return t;
    }

    inline Typeface::Ptr monoTypeface()
    {
        static auto t = Typeface::createSystemTypefaceFor (BinaryData::IBMPlexMonoMedium_ttf, BinaryData::IBMPlexMonoMedium_ttfSize);
        return t;
    }

    inline Font title (float height) { return Font (FontOptions().withTypeface (titleTypeface()).withHeight (height)); }
    inline Font mono (float height)  { return Font (FontOptions().withTypeface (monoTypeface()).withHeight (height)); }
    inline Font body (float height, bool bold = false) { return Font (FontOptions (height, bold ? Font::bold : Font::plain)); }

    // Flat, rounded, dark controls with the brand colours.
    class LookAndFeel : public LookAndFeel_V4
    {
    public:
        LookAndFeel()
            : LookAndFeel_V4 (ColourScheme (bg, panel, raised, outline, text, primary, Colours::white, raised, text))
        {
            setColour (ResizableWindow::backgroundColourId, bg);
            setColour (TextButton::buttonColourId, raised);
            setColour (TextButton::buttonOnColourId, primary);
            setColour (TextButton::textColourOffId, text);
            setColour (TextButton::textColourOnId, Colours::white);
            setColour (ComboBox::backgroundColourId, raised);
            setColour (ComboBox::outlineColourId, outline);
            setColour (ComboBox::arrowColourId, muted);
            setColour (PopupMenu::backgroundColourId, raised);
            setColour (PopupMenu::highlightedBackgroundColourId, primary.withAlpha (0.85f));
            setColour (PopupMenu::headerTextColourId, muted);
            setColour (Slider::thumbColourId, Colours::white);
            setColour (Slider::trackColourId, primary);
            setColour (Slider::backgroundColourId, outline);
            setColour (Slider::textBoxTextColourId, text);
            setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
            setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
            setColour (Label::textColourId, text);
            setColour (TextEditor::backgroundColourId, raised);
            setColour (TextEditor::outlineColourId, outline);
            setColour (TextEditor::focusedOutlineColourId, primary);
            setColour (TextEditor::highlightColourId, primary.withAlpha (0.4f));
            setColour (ListBox::backgroundColourId, panel);
            setColour (ScrollBar::thumbColourId, outline.brighter (0.3f));
            setColour (ProgressBar::foregroundColourId, primary);
            setColour (ProgressBar::backgroundColourId, raised);
            setColour (AlertWindow::backgroundColourId, panel);
            setColour (ToggleButton::tickColourId, primary);
            setColour (TooltipWindow::backgroundColourId, raised);
            setColour (TooltipWindow::outlineColourId, outline);
            setColour (DocumentWindow::textColourId, text);
            setColour (CaretComponent::caretColourId, primary);
        }

        void drawButtonBackground (Graphics& g, Button& b, const Colour& base, bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            auto c = base.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.45f);
            if (down)      c = c.brighter (0.15f);
            else if (over) c = c.brighter (0.08f);
            g.setColour (c);
            g.fillRoundedRectangle (r, radius);
            if (base == findColour (TextButton::buttonColourId))
            {
                g.setColour (outline);
                g.drawRoundedRectangle (r, radius, 1.0f);
            }
        }

        Font getTextButtonFont (TextButton&, int h) override { return body (jmin (14.0f, h * 0.45f), true); }
        Font getComboBoxFont (ComboBox&) override { return body (14.0f); }
        Font getPopupMenuFont() override { return body (15.0f); }
        Font getLabelFont (Label& l) override { return l.getFont(); }

        void drawComboBox (Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& box) override
        {
            auto r = Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
            g.setColour (raised.withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
            g.fillRoundedRectangle (r, radius);
            g.setColour (box.hasKeyboardFocus (true) ? primary : outline);
            g.drawRoundedRectangle (r, radius, 1.0f);
            Path arrow;
            const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
            arrow.startNewSubPath (cx - 4, cy - 2);
            arrow.lineTo (cx, cy + 2);
            arrow.lineTo (cx + 4, cy - 2);
            g.setColour (muted);
            g.strokePath (arrow, PathStrokeType (1.5f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        void positionComboBoxText (ComboBox& box, Label& label) override
        {
            label.setBounds (8, 1, box.getWidth() - 28, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }

        void drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float,
                               Slider::SliderStyle style, Slider& s) override
        {
            if (style == Slider::LinearVertical)
            {
                // Console fader: thin slot, filled from the bottom, a cap with a line.
                const float cx = (float) x + (float) w * 0.5f;
                auto slot = Rectangle<float> (cx - 2, (float) y, 4, (float) h);
                g.setColour (lane);
                g.fillRoundedRectangle (slot, 2);
                g.setGradientFill (ColourGradient (accent, cx, (float) (y + h), primary, cx, (float) y, false));
                g.fillRoundedRectangle (slot.withTop (pos), 2);
                auto cap = Rectangle<float> (jmin (30.0f, (float) w - 4), 14).withCentre ({ cx, pos });
                g.setGradientFill (ColourGradient (Colour (0xffd4d4d8), cap.getX(), cap.getY(), Colour (0xff8a8a93), cap.getX(), cap.getBottom(), false));
                g.fillRoundedRectangle (cap, 3);
                g.setColour (bg);
                g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 8, 1.5f));
                return;
            }
            if (style != Slider::LinearHorizontal)
                return LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
            const float cy = (float) y + (float) h * 0.5f;
            const float track = 4.0f;
            auto bgTrack = Rectangle<float> ((float) x, cy - track / 2, (float) w, track);
            g.setColour (outline);
            g.fillRoundedRectangle (bgTrack, track / 2);
            // Filled from the centre for symmetric sliders (pan, pitch), from the left otherwise (volume, speed).
            const bool bipolar = s.getMinimum() < 0 && approximatelyEqual (-s.getMinimum(), s.getMaximum()); // pan, pitch
            const float from = bipolar ? (float) s.getPositionOfValue (0.0) : (float) x;
            g.setColour (s.isEnabled() ? primary : muted);
            g.fillRoundedRectangle (Rectangle<float> (jmin (from, pos), cy - track / 2, std::abs (pos - from), track), track / 2);
            g.setColour (s.isEnabled() ? Colours::white : muted);
            g.fillEllipse (pos - 7, cy - 7, 14, 14);
        }

        // Knob: a track arc, the value arc (from the centre for pan), and a pointer.
        void drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider& s) override
        {
            auto r = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3);
            const float radiusK = jmin (r.getWidth(), r.getHeight()) / 2;
            const auto c = r.getCentre();
            const float angle = start + pos * (end - start);
            const bool bipolar = s.getMinimum() < 0 && approximatelyEqual (-s.getMinimum(), s.getMaximum());
            const float from = bipolar ? (start + end) / 2 : start;

            g.setColour (raised.brighter (0.15f));
            g.fillEllipse (Rectangle<float> (radiusK * 2 - 6, radiusK * 2 - 6).withCentre (c));
            Path track, value;
            track.addCentredArc (c.x, c.y, radiusK, radiusK, 0, start, end, true);
            value.addCentredArc (c.x, c.y, radiusK, radiusK, 0, jmin (from, angle), jmax (from, angle), true);
            g.setColour (outline.brighter (0.2f));
            g.strokePath (track, PathStrokeType (2.5f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (s.isEnabled() ? primary : muted);
            g.strokePath (value, PathStrokeType (2.5f, PathStrokeType::curved, PathStrokeType::rounded));
            Path pointer;
            pointer.addRoundedRectangle (-1.25f, -(radiusK - 4), 2.5f, radiusK * 0.55f, 1.0f);
            g.setColour (text);
            g.fillPath (pointer, AffineTransform::rotation (angle).translated (c.x, c.y));
        }

        void drawPopupMenuBackground (Graphics& g, int w, int h) override
        {
            g.fillAll (raised);
            g.setColour (outline);
            g.drawRect (0, 0, w, h);
        }

        void drawProgressBar (Graphics& g, ProgressBar& bar, int w, int h, double progress, const String& text) override
        {
            if (progress < 0 || progress > 1)
                return LookAndFeel_V4::drawProgressBar (g, bar, w, h, progress, text);
            auto r = Rectangle<float> (0, 0, (float) w, (float) h).reduced (0, (float) h * 0.3f);
            g.setColour (raised);
            g.fillRoundedRectangle (r, r.getHeight() / 2);
            g.setColour (primary);
            g.fillRoundedRectangle (r.withWidth (r.getWidth() * (float) progress), r.getHeight() / 2);
        }
    };

    // Small line icons for the header buttons.
    enum class Icon { songs, projects, import, exportIcon, settings };

    inline Path iconPath (Icon icon, Rectangle<float> r)
    {
        Path p;
        const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
        switch (icon)
        {
            case Icon::songs: // two notes
                p.addEllipse (x, y + h * 0.65f, w * 0.38f, h * 0.32f);
                p.addEllipse (x + w * 0.58f, y + h * 0.55f, w * 0.38f, h * 0.32f);
                p.addRectangle (x + w * 0.3f, y + h * 0.12f, w * 0.09f, h * 0.7f);
                p.addRectangle (x + w * 0.88f, y + h * 0.02f, w * 0.09f, h * 0.7f);
                p.addQuadrilateral (x + w * 0.3f, y + h * 0.12f, x + w * 0.97f, y + h * 0.02f, x + w * 0.97f, y + h * 0.2f, x + w * 0.3f, y + h * 0.3f);
                break;
            case Icon::projects: // a folder
                p.addRoundedRectangle (x, y + h * 0.12f, w * 0.45f, h * 0.2f, h * 0.06f);
                p.addRoundedRectangle (x, y + h * 0.25f, w, h * 0.65f, h * 0.08f);
                break;
            case Icon::import: // arrow down into a tray
            case Icon::exportIcon: // arrow up out of a tray
            {
                const bool down = icon == Icon::import;
                p.addRectangle (x, y + h * 0.82f, w, h * 0.12f);
                p.addRectangle (x, y + h * 0.55f, w * 0.12f, h * 0.3f);
                p.addRectangle (x + w * 0.88f, y + h * 0.55f, w * 0.12f, h * 0.3f);
                p.addRectangle (x + w * 0.44f, y + (down ? 0.0f : h * 0.22f), w * 0.12f, h * 0.48f);
                if (down) p.addTriangle (x + w * 0.22f, y + h * 0.42f, x + w * 0.78f, y + h * 0.42f, x + w * 0.5f, y + h * 0.72f);
                else      p.addTriangle (x + w * 0.22f, y + h * 0.3f, x + w * 0.78f, y + h * 0.3f, x + w * 0.5f, y);
                break;
            }
            case Icon::settings: // three sliders
                for (int i = 0; i < 3; ++i)
                {
                    const float ly = y + h * (0.15f + 0.35f * (float) i);
                    p.addRectangle (x, ly - h * 0.05f, w, h * 0.1f);
                    const float kx = x + w * (i == 1 ? 0.65f : 0.25f);
                    p.addEllipse (kx - w * 0.13f, ly - h * 0.13f, w * 0.26f, h * 0.26f);
                }
                break;
        }
        return p;
    }

    // A header button with an icon before its text.
    class IconButton : public TextButton
    {
    public:
        IconButton (const String& text, Icon i) : TextButton (text), icon (i) {}

        void paintButton (Graphics& g, bool over, bool down) override
        {
            getLookAndFeel().drawButtonBackground (g, *this, findColour (buttonColourId), over, down);
            auto r = getLocalBounds().toFloat().reduced (12, 0);
            const auto font = body (14.0f, true);
            const float textW = GlyphArrangement::getStringWidth (font, getButtonText());
            const float iconS = 15.0f, gap = 8.0f;
            auto content = r.withSizeKeepingCentre (jmin (r.getWidth(), iconS + gap + textW), r.getHeight());
            g.setColour (Theme::primary.brighter (0.3f));
            g.fillPath (iconPath (icon, content.removeFromLeft (iconS).withSizeKeepingCentre (iconS, iconS)));
            content.removeFromLeft (gap);
            g.setColour (Theme::text.withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
            g.setFont (font);
            g.drawText (getButtonText(), content, Justification::centredLeft, false);
        }

    private:
        Icon icon;
    };
}
