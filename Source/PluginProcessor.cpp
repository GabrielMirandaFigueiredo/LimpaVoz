#include "PluginProcessor.h"

LimpaVozProcessor::LimpaVozProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Entrada", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Saida",   juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETROS", createLayout())
{
    mixParam     = apvts.getRawParameterValue ("mix");
    lowCutParam  = apvts.getRawParameterValue ("lowcut");
    gainParam    = apvts.getRawParameterValue ("gain");
    bypassParam  = apvts.getRawParameterValue ("bypass");
}

juce::AudioProcessorValueTreeState::ParameterLayout LimpaVozProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "mix", 1 }, "Intensidade da limpeza",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "lowcut", 1 }, "Corte de graves",
        NormalisableRange<float> (20.0f, 300.0f, 1.0f, 0.5f), 70.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "gain", 1 }, "Ganho de saida",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "bypass", 1 }, "Desligar limpeza (bypass)", false));

    return { params.begin(), params.end() };
}

bool LimpaVozProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void LimpaVozProcessor::prepareToPlay (double sampleRate, int)
{
    for (auto& c : cleaners)
        c.prepare (sampleRate);

    gainSmoothed.reset (sampleRate, 0.02);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParam->load()));

    // informa ao host a latência do processamento, para ele compensar automaticamente
    setLatencySamples (cleaners[0].getLatencySamples());
}

void LimpaVozProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min (buffer.getNumChannels(), (int) cleaners.size());

    const bool bypass = bypassParam->load() > 0.5f;
    // Em bypass continuamos passando pelo atraso (mix = 0) para manter a latência constante.
    const float mix = bypass ? 0.0f : mixParam->load() * 0.01f;
    const float lowCut = bypass ? 20.0f : lowCutParam->load();

    gainSmoothed.setTargetValue (bypass ? 1.0f : juce::Decibels::decibelsToGain (gainParam->load()));

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        cleaners[(size_t) ch].process (data, data, numSamples, mix, lowCut, 1.0f);
    }

    // ganho de saída com suavização (evita estalos ao mexer no controle)
    if (gainSmoothed.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gainSmoothed.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain (gainSmoothed.getTargetValue());
    }
}

juce::AudioProcessorEditor* LimpaVozProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void LimpaVozProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void LimpaVozProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LimpaVozProcessor();
}
