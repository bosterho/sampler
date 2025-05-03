#include "PluginEditor.h"

PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    addAndMakeVisible (inspectButton);
    addAndMakeVisible (fileNameLabel);
    addAndMakeVisible (loadingStatusLabel);
    
    // Initial filename display
    juce::String initialPath = processorRef.getLoadedFilePath();
    if (initialPath.isNotEmpty())
    {
        juce::File file(initialPath);
        fileNameLabel.setText(file.getFileName(), juce::dontSendNotification);
        
        // Initialize loading status
        auto* sound = processorRef.getCurrentStreamingSound();
        if (sound != nullptr)
        {
            if (sound->isFullyLoaded())
                loadingStatusLabel.setText("Sample fully loaded", juce::dontSendNotification);
            else
                loadingStatusLabel.setText("Initial 60KB loaded. Play to load full sample...", juce::dontSendNotification);
        }
    }
    else
    {
        fileNameLabel.setText("No Sample Loaded - Using Default", juce::dontSendNotification);
        loadingStatusLabel.setText("", juce::dontSendNotification);
    }

    // this chunk of code instantiates and opens the melatonin inspector
    inspectButton.onClick = [&] {
        if (!inspector)
        {
            inspector = std::make_unique<melatonin::Inspector> (*this);
            inspector->onClose = [this]() { inspector.reset(); };
        }

        inspector->setVisible (true);
    };

    // Start a timer to update the loading status
    startTimerHz(10);

    // Make sure that before the constructor has finished, you've set the
    // editor's size to whatever you need it to be.
    setSize (500, 400);
}

PluginEditor::~PluginEditor()
{
    stopTimer();
}

void PluginEditor::paint (juce::Graphics& g)
{
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    auto area = getLocalBounds();
    g.setColour (juce::Colours::white);
    g.setFont (16.0f);
    auto helloWorld = juce::String ("Sampler Plugin ") + PRODUCT_NAME_WITHOUT_VERSION + " v" VERSION;
    g.drawText (helloWorld, area.removeFromTop (50), juce::Justification::centred, false);
    
    g.setFont(14.0f);
    g.drawText("Drop audio files here to load samples", area.removeFromTop(30), juce::Justification::centred, false);
}

void PluginEditor::resized()
{
    auto area = getLocalBounds().reduced(20);
    
    auto topArea = area.removeFromTop(160);
    inspectButton.setBounds(topArea.removeFromBottom(50).withSizeKeepingCentre(100, 40));
    
    fileNameLabel.setBounds(area.removeFromTop(50).withSizeKeepingCentre(300, 30));
    loadingStatusLabel.setBounds(area.removeFromTop(30).withSizeKeepingCentre(300, 30));
}

bool PluginEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto file : files)
    {
        if (file.endsWith(".wav") || file.endsWith(".aif") || file.endsWith(".aiff"))
            return true;
    }
    
    return false;
}

void PluginEditor::filesDropped(const juce::StringArray& files, int x, int y)
{
    for (auto file : files)
    {
        if (file.endsWith(".wav") || file.endsWith(".aif") || file.endsWith(".aiff"))
        {
            processorRef.loadFile(file);
            juce::File loadedFile(file);
            fileNameLabel.setText(loadedFile.getFileName(), juce::dontSendNotification);
            break;
        }
    }
}

void PluginEditor::timerCallback()
{
    // Update loading status
    auto* sound = processorRef.getCurrentStreamingSound();
    if (sound != nullptr)
    {
        if (sound->isFullyLoaded())
        {
            loadingStatusLabel.setText("Sample fully loaded", juce::dontSendNotification);
        }
        else if (sound->isLoading())
        {
            loadingStatusLabel.setText("Loading complete sample...", juce::dontSendNotification);
        }
        else
        {
            loadingStatusLabel.setText("Initial 60KB loaded. Play to load full sample...", juce::dontSendNotification);
        }
    }
    else
    {
        loadingStatusLabel.setText("", juce::dontSendNotification);
    }
}
