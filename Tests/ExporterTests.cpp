#include <JuceHeader.h>
#include "../Source/Exporter.h"
#include "TestAudio.h"

// Real offline renders: the file must sound like playback (mute respected, speed applied).
struct ExporterTests : UnitTest
{
    ExporterTests() : UnitTest ("Exporter") {}

    String exportAndWait (te::Edit& edit, Array<Exporter::Item> items, Exporter::Format f, Array<int>* progress = nullptr)
    {
        String error = "timeout";
        bool done = false;
        Exporter ex (edit, items, f, [&] (int d, int) { if (progress) progress->add (d); },
                     [&] (const String& e) { error = e; done = true; });
        ex.start();
        for (int i = 0; i < 3000 && ! done; ++i)
            MessageManager::getInstance()->runDispatchLoopUntil (10);
        return error;
    }

    void runTest() override
    {
        te::Engine engine { "VelodyTests" };
        auto dir = File::createTempFile ("velody-export");
        TestAudio::writeSine (dir.getChildFile ("bass.wav"), 2.0, 220);
        TestAudio::writeSine (dir.getChildFile ("vocals.wav"), 2.0, 880);
        TestAudio::writeSine (dir.getChildFile ("click.wav"), 2.0, 3000);

        auto edit = SongEdit::openOrCreate (engine, dir.getChildFile ("song.tracktionedit"));
        SongEdit::addStemTracks (*edit, { dir.getChildFile ("bass.wav"), dir.getChildFile ("vocals.wav") });
        SongEdit::setClickTrack (*edit, dir.getChildFile ("click.wav"));
        edit->ensureNumberOfAudioTracks (4); // an empty input track
        auto tracks = te::getAudioTracks (*edit);

        beginTest ("exportable tracks: stems with audio, not the click or empty input tracks");
        auto ex = Exporter::exportableTracks (*edit);
        expectEquals (ex.size(), 2);
        expect (ex.contains (tracks[0]) && ex.contains (tracks[1]));

        beginTest ("mix WAV sounds like playback: muted tracks (vocals, click) are left out");
        tracks[1]->setMute (true);
        tracks[2]->setMute (true);
        auto mix = dir.getChildFile ("mix.wav");
        Array<Exporter::Item> one;
        one.add ({ {}, mix });
        expectEquals (exportAndWait (*edit, one, Exporter::Format::wav), String());
        auto [len, freq] = TestAudio::analyse (mix);
        expectWithinAbsoluteError (len, 2.0, 0.1);
        expectWithinAbsoluteError (freq, 220.0, 10.0);

        beginTest ("mix follows the practice speed (as heard), and re-exporting replaces the old file");
        SongEdit::setPractice (*edit, 0.5, 0);
        expectEquals (exportAndWait (*edit, one, Exporter::Format::wav), String());
        std::tie (len, freq) = TestAudio::analyse (mix);
        expectGreaterThan (len, 3.9);
        expectWithinAbsoluteError (freq, 220.0, 10.0);
        SongEdit::setPractice (*edit, 1.0, 0);
        tracks[1]->setMute (false);

        beginTest ("each track on its own: one file per stem, only that stem in it");
        Array<Exporter::Item> stems;
        for (auto t : Exporter::exportableTracks (*edit))
            stems.add (Exporter::stemItem (*edit, *t, dir.getChildFile (t->getName() + ".out.wav")));
        Array<int> progress;
        expectEquals (exportAndWait (*edit, stems, Exporter::Format::wav, &progress), String());
        expect (progress == Array<int> { 0, 1 });
        expectWithinAbsoluteError (TestAudio::analyse (dir.getChildFile ("bass.out.wav")).second, 220.0, 10.0);
        expectWithinAbsoluteError (TestAudio::analyse (dir.getChildFile ("vocals.out.wav")).second, 880.0, 20.0);

        beginTest ("MP3: converted with ffmpeg, temp WAV removed");
        auto mp3 = dir.getChildFile ("mix.mp3");
        Array<Exporter::Item> m;
        m.add ({ {}, mp3 });
        const auto tempsBefore = File::getSpecialLocation (File::tempDirectory).findChildFiles (File::findFiles, false, "velody-export*.wav").size();
        expectEquals (exportAndWait (*edit, m, Exporter::Format::mp3), String());
        expect (mp3.existsAsFile());
        te::AudioFile decoded (engine, mp3);
        expectWithinAbsoluteError (decoded.getLength(), 2.0, 0.15);
        expectEquals (File::getSpecialLocation (File::tempDirectory).findChildFiles (File::findFiles, false, "velody-export*.wav").size(), tempsBefore);

        edit = nullptr;
        dir.deleteRecursively();
    }
};

static ExporterTests exporterTests;
