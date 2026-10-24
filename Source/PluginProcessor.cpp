#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

class PrometheanProcessor : public juce::AudioProcessor
{
public:
    PrometheanProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          apvts(*this, nullptr, "PARAMS", createLayout()),
          oversampler(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true)
    {
    }

    //==============================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        std::vector<std::unique_ptr<RangedAudioParameter>> p;

        auto eq = [&](const char* id, const char* name)
        {
            p.push_back(std::make_unique<AudioParameterFloat>(
                ParameterID{ id, 1 }, name, NormalisableRange<float>(-15.0f, 15.0f, 0.1f), 0.0f));
        };

        p.push_back(std::make_unique<AudioParameterBool>(ParameterID{ "pad", 1 }, "-10dB Pad", false));
        p.push_back(std::make_unique<AudioParameterFloat>(
            ParameterID{ "drive", 1 }, "Drive", NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.3f));
        eq("low", "Low");
        eq("lowmid", "Low Mid");
        eq("highmid", "High Mid");
        eq("high", "High");
        p.push_back(std::make_unique<AudioParameterBool>(ParameterID{ "cab", 1 }, "Cabinet", true));
        p.push_back(std::make_unique<AudioParameterFloat>(
            ParameterID{ "master", 1 }, "Master", NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.7f));

        return { p.begin(), p.end() };
    }

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        sr = sampleRate;

        juce::dsp::ProcessSpec spec{ sampleRate, (juce::uint32) samplesPerBlock, 2 };
        for (auto* f : { &lowF, &lowMidF, &highMidF, &highF, &cabHP, &cabLP })
        {
            f->prepare(spec);
            f->reset();
        }

        oversampler.initProcessing((size_t) samplesPerBlock);
        oversampler.reset();
        setLatencySamples((int) oversampler.getLatencyInSamples());

        updateFilters();
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        auto out = l.getMainOutputChannelSet();
        return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
               && out == l.getMainInputChannelSet();
    }

    void updateFilters()
    {
        using C = juce::dsp::IIR::Coefficients<float>;
        auto g = [&](const char* id) { return juce::Decibels::decibelsToGain(apvts.getRawParameterValue(id)->load()); };

        *lowF.state     = *C::makeLowShelf(sr, 80.0f, 0.7f, g("low"));
        *lowMidF.state  = *C::makePeakFilter(sr, 400.0f, 0.8f, g("lowmid"));
        *highMidF.state = *C::makePeakFilter(sr, 1500.0f, 0.8f, g("highmid"));
        *highF.state    = *C::makeHighShelf(sr, 4500.0f, 0.7f, g("high"));

        // Speaker-Simulation (10" Woofer + Tweeter)
        *cabHP.state = *C::makeHighPass(sr, 45.0f, 0.8f);
        *cabLP.state = *C::makeLowPass(sr, 9000.0f, 0.7f);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int numCh = juce::jmin(buffer.getNumChannels(), 2);
        if (numCh == 0)
            return;

        updateFilters();

        const bool pad = apvts.getRawParameterValue("pad")->load() > 0.5f;
        const bool cab = apvts.getRawParameterValue("cab")->load() > 0.5f;
        const float drive = apvts.getRawParameterValue("drive")->load();
        const float master = apvts.getRawParameterValue("master")->load();

        const float padGain = pad ? juce::Decibels::decibelsToGain(-10.0f) : 1.0f;
        const float preGain = juce::Decibels::decibelsToGain(drive * 24.0f);
        const float masterGain = master < 0.001f ? 0.0f
                                                 : juce::Decibels::decibelsToGain(-60.0f + master * 66.0f);

        juce::dsp::AudioBlock<float> block(buffer.getArrayOfWritePointers(), (size_t) numCh,
                                           (size_t) buffer.getNumSamples());

        // Input + Pad + Drive
        block.multiplyBy(padGain * preGain);

        // 4-Band-EQ
        juce::dsp::ProcessContextReplacing<float> ctx(block);
        lowF.process(ctx);
        lowMidF.process(ctx);
        highMidF.process(ctx);
        highF.process(ctx);

        // Saettigung (oversampled)
        auto up = oversampler.processSamplesUp(block);
        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            auto* d = up.getChannelPointer(ch);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                float x = d[i];
                // leicht asymmetrische Saettigung (Roehren-/Endstufen-Charakter)
                float y = x >= 0.0f ? std::tanh(x) : std::tanh(x * 0.9f) / 0.9f * 0.95f;
                d[i] = y;
            }
        }
        oversampler.processSamplesDown(block);

        // Cabinet
        if (cab)
        {
            cabHP.process(ctx);
            cabLP.process(ctx);
        }

        // Master
        block.multiplyBy(masterGain);
    }

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override
    {
        return new juce::GenericAudioProcessorEditor(*this);
    }
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Promethean"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary(data, size))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                                  juce::dsp::IIR::Coefficients<float>>;

    juce::AudioProcessorValueTreeState apvts;
    juce::dsp::Oversampling<float> oversampler;
    Filter lowF, lowMidF, highMidF, highF, cabHP, cabLP;
    double sr = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PrometheanProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PrometheanProcessor();
}
