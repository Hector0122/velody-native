#pragma once
#include <JuceHeader.h>

namespace AppSettings
{
    // Settings used to land in ~/Velody/Velody.settings (JUCE puts folderName under ~ on Linux): into
    // ~/.config/Velody. When both exist (builds wrote to each), they're merged, the old file's values
    // winning (it's the one the installed app kept using); the old file goes.
    inline bool migrate (const File& oldFile, const File& newFile)
    {
        if (! oldFile.existsAsFile())
            return false;
        newFile.getParentDirectory().createDirectory();
        auto merged = newFile.existsAsFile() ? XmlDocument::parse (newFile) : nullptr;
        if (merged == nullptr || ! merged->hasTagName ("PROPERTIES"))
            merged = std::make_unique<XmlElement> ("PROPERTIES");
        if (auto old = XmlDocument::parse (oldFile))
            for (auto* v : old->getChildWithTagNameIterator ("VALUE"))
            {
                for (auto* existing : merged->getChildWithTagNameIterator ("VALUE"))
                    if (existing->getStringAttribute ("name") == v->getStringAttribute ("name"))
                    {
                        merged->removeChildElement (existing, true);
                        break;
                    }
                merged->addChildElement (new XmlElement (*v));
            }
        return merged->writeTo (newFile) && oldFile.deleteFile();
    }
}
