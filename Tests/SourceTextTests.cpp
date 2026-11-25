#include <JuceHeader.h>
#include <regex>

// JUCE reads plain "…" literals as Latin-1, so "canción" shows as "canciÃ³n" on screen. Every string
// literal with accents (or any non-ASCII) in the app's sources must be u8"…". This guards it.
struct SourceTextTests : UnitTest
{
    SourceTextTests() : UnitTest ("Source text encoding") {}

    void runTest() override
    {
        beginTest ("no non-ASCII string literal without u8 in Source/");
        const auto source = File (VELODY_PYTHON_WORKER_DIR).getSiblingFile ("Source");
        expect (source.isDirectory());
        static const std::regex literal (R"("(?:[^"\\\n]|\\.)*")");
        int checked = 0;
        for (auto& f : source.findChildFiles (File::findFiles, true, "*.h;*.cpp"))
        {
            StringArray lines;
            f.readLines (lines);
            for (int i = 0; i < lines.size(); ++i)
            {
                const auto line = lines[i].toStdString();
                if (lines[i].trimStart().startsWith ("//"))
                    continue;
                for (std::sregex_iterator it (line.begin(), line.end(), literal), end; it != end; ++it)
                {
                    const auto pos = (size_t) it->position();
                    const bool u8 = pos >= 2 && line.compare (pos - 2, 2, "u8") == 0;
                    const auto text = it->str();
                    const bool ascii = std::all_of (text.begin(), text.end(), [] (char c) { return (unsigned char) c < 128; });
                    ++checked;
                    expect (u8 || ascii, f.getFileName() + ":" + String (i + 1) + "  " + lines[i].trim());
                }
            }
        }
        expectGreaterThan (checked, 500);
    }
};

static SourceTextTests sourceTextTests;
