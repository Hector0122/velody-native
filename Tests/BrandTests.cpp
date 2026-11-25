#include <JuceHeader.h>
#include "../Source/ui/Brand.h"

// The app icon is drawn from the embedded SVG (gradients included). VELODY_ICON_OUT=<file.png> also
// writes it, which is how packaging/velody-native.png is made.
struct BrandTests : UnitTest
{
    BrandTests() : UnitTest ("Brand") {}

    void runTest() override
    {
        beginTest ("icon: the blue→violet square with white stems, transparent corners");
        auto img = Brand::icon (512);
        expect (img.getPixelAt (2, 2).getAlpha() == 0);                 // rounded corner
        auto topLeft = img.getPixelAt (60, 60), bottomRight = img.getPixelAt (450, 450);
        expect (topLeft.getBlue() > topLeft.getRed() && topLeft.getAlpha() == 255, topLeft.toDisplayString (true));
        expect (bottomRight.getRed() > topLeft.getRed(), bottomRight.toDisplayString (true)); // towards violet
        expect (img.getPixelAt (256, 300).getBrightness() > 0.95f);    // the middle stem

        if (auto out = SystemStats::getEnvironmentVariable ("VELODY_ICON_OUT", {}); out.isNotEmpty())
        {
            File f (out);
            f.deleteFile();
            FileOutputStream stream (f);
            expect (PNGImageFormat().writeImageToStream (img, stream));
        }
    }
};

static BrandTests brandTests;
