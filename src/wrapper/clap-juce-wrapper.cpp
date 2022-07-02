/*
 * BaconPaul's running todo
 *
 * - We always say we are an instrument....
 * - midi out (try stochas perhaps?)
 * - why does dexed not work?
 * - Finish populating the desc
 * - Cleanup and comment of course (including the CMake) including what's skipped
 */

#if _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/format_types/juce_LegacyAudioParameter.cpp>

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wunused-parameter", "-Wsign-conversion", "-Wfloat-conversion")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC(4100 4127 4244)
// Sigh - X11.h eventually does a #define None 0L which doesn't work
// with an enum in clap land being called None, so just undef it
// post the JUCE installs
#ifdef None
#undef None
#endif
#include <clap/helpers/checking-level.hh>
#include <clap/helpers/host-proxy.hh>
#include <clap/helpers/host-proxy.hxx>
#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>
JUCE_END_IGNORE_WARNINGS_MSVC
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

#include <clap-juce-extensions/clap-juce-extensions.h>

#if JUCE_LINUX
#if JUCE_VERSION > 0x060008
#include <juce_audio_plugin_client/utility/juce_LinuxMessageThread.h>
#endif
#endif

#define FIXME(x)                                                                                   \
    {                                                                                              \
        static bool onetime_ = false;                                                              \
        if (!onetime_)                                                                             \
        {                                                                                          \
            std::ostringstream oss;                                                                \
            oss << "FIXME: " << x << " @" << __LINE__;                                             \
            DBG(oss.str());                                                                        \
        }                                                                                          \
        jassert(onetime_);                                                                         \
        onetime_ = true;                                                                           \
    }

/*
 * This is a utility lock free queue based on the JUCE abstract fifo
 */
template <typename T, int qSize = 4096> class PushPopQ
{
  public:
    PushPopQ() : af(qSize) {}
    bool push(const T &ad)
    {
        auto ret = false;
        int start1, size1, start2, size2;
        af.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 > 0)
        {
            dq[start1] = ad;
            ret = true;
        }
        af.finishedWrite(size1 + size2);
        return ret;
    }
    bool pop(T &ad)
    {
        bool ret = false;
        int start1, size1, start2, size2;
        af.prepareToRead(1, start1, size1, start2, size2);
        if (size1 > 0)
        {
            ad = dq[start1];
            ret = true;
        }
        af.finishedRead(size1 + size2);
        return ret;
    }
    juce::AbstractFifo af;
    T dq[(size_t)qSize];
};

/*
 * These functions are the JUCE VST2/3 NSView attachment functions. We compile them into
 * our clap dll by, on macos, also linking clap_juce_mac.mm
 */
namespace juce
{
extern JUCE_API void initialiseMacVST();
extern JUCE_API void *attachComponentToWindowRefVST(Component *, void *parentWindowOrView,
                                                    bool isNSView);
} // namespace juce

JUCE_BEGIN_IGNORE_WARNINGS_MSVC(4996) // allow strncpy

#if !defined(CLAP_MISBEHAVIOUR_HANDLER_LEVEL)
#define CLAP_MISBEHAVIOUR_HANDLER_LEVEL "Ignore"
#endif

#if !defined(CLAP_CHECKING_LEVEL)
#define CLAP_CHECKING_LEVEL "Minimal"
#endif

// This is useful for debugging overrides
// #undef CLAP_MISBEHAVIOUR_HANDLER_LEVEL
// #define CLAP_MISBEHAVIOUR_HANDLER_LEVEL Terminate
// #undef CLAP_CHECKING_LEVEL
// #define CLAP_CHECKING_LEVEL Maximal

/*
 * A little class that sets an atomic bool to a value across its lifetime and
 * restores it on exit.
 */
template <typename T> struct AtomicTGuard
{
    std::atomic<T> &ref;
    T valAtConstruct;
    AtomicTGuard(std::atomic<T> &b, T val) : ref(b), valAtConstruct(b) { ref = val; }
    ~AtomicTGuard() { ref = valAtConstruct; }
};

/*
 * The ClapJuceWrapper is a class which immplements a collection
 * of CLAP and JUCE APIs
 */
class ClapJuceWrapper : public clap::helpers::Plugin<
                            clap::helpers::MisbehaviourHandler::CLAP_MISBEHAVIOUR_HANDLER_LEVEL,
                            clap::helpers::CheckingLevel::CLAP_CHECKING_LEVEL>,
                        public juce::AudioProcessorListener,
                        public juce::AudioPlayHead,
                        public juce::AudioProcessorParameter::Listener,
                        public juce::ComponentListener
{
  public:
    // this needs to be the very last thing to get deleted!
    juce::ScopedJuceInitialiser_GUI libraryInitializer;

    static clap_plugin_descriptor desc;
    std::unique_ptr<juce::AudioProcessor> processor;
    clap_juce_extensions::clap_properties *processorAsClapProperties{nullptr};
    clap_juce_extensions::clap_juce_audio_processor_capabilities *processorAsClapExtensions{
        nullptr};

    bool usingLegacyParameterAPI{false};

    ClapJuceWrapper(const clap_host *host, juce::AudioProcessor *p)
        : clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::CLAP_MISBEHAVIOUR_HANDLER_LEVEL,
                                clap::helpers::CheckingLevel::CLAP_CHECKING_LEVEL>(&desc, host),
          processor(p)
    {
        processor->setRateAndBufferSizeDetails(0, 0);
        processor->setPlayHead(this);
        processor->addListener(this);

        processorAsClapProperties = dynamic_cast<clap_juce_extensions::clap_properties *>(p);
        processorAsClapExtensions =
            dynamic_cast<clap_juce_extensions::clap_juce_audio_processor_capabilities *>(p);

        const bool forceLegacyParamIDs = false;

        juceParameters.update(*processor, forceLegacyParamIDs);

        if (processor->getParameters().size() == 0)
        {
            usingLegacyParameterAPI = true;
            DBG("Using Legacy Parameter API: getText will ignore value and use plugin value.");
        }

        for (auto *juceParam :
#if JUCE_VERSION >= 0x060103
             juceParameters
#else
             juceParameters.params
#endif

        )
        {
            uint32_t clapID = generateClapIDForJuceParam(juceParam);

            allClapIDs.insert(clapID);
            paramPtrByClapID[clapID] = juceParam;
            clapIDByParamPtr[juceParam] = clapID;
        }
    }

    ~ClapJuceWrapper() override
    {
        processor->editorBeingDeleted(editor.get());

#if JUCE_LINUX
        if (_host.canUseTimerSupport())
        {
            _host.timerSupportUnregister(idleTimer);
        }
#endif
    }

    bool init() noexcept override
    {
#if JUCE_LINUX
        if (_host.canUseTimerSupport())
        {
            _host.timerSupportRegister(1000 / 50, &idleTimer);
        }
#endif
        defineAudioPorts();

        return true;
    }

  public:
    bool implementsTimerSupport() const noexcept override { return true; }
    void onTimer(clap_id timerId) noexcept override
    {
        juce::ignoreUnused(timerId);
#if LINUX
        juce::ScopedJuceInitialiser_GUI libraryInitialiser;
        const juce::MessageManagerLock mmLock;

#if JUCE_VERSION > 0x060008
        while (juce::dispatchNextMessageOnSystemQueue(true))
        {
        }
#else
        auto mm = juce::MessageManager::getInstance();
        mm->runDispatchLoopUntil(0);
#endif
#endif
    }

    clap_id idleTimer{0};

    uint32_t generateClapIDForJuceParam(juce::AudioProcessorParameter *param) const
    {
        auto juceParamID = juce::LegacyAudioParameter::getParamID(param, false);
        auto clapID = static_cast<uint32_t>(juceParamID.hashCode());
        return clapID;
    }

#if JUCE_VERSION >= 0x060008
    void audioProcessorChanged(juce::AudioProcessor *proc, const ChangeDetails &details) override
    {
        juce::ignoreUnused(proc);
        if (details.latencyChanged)
        {
            runOnMainThread([this] {
                if (isBeingDestroyed())
                    return;

                if (_host.canUseLatency())
                    _host.latencyChanged();
            });
        }
        if (details.programChanged)
        {
            // At the moment, CLAP doesn't have a sense of programs (to my knowledge).
            // (I think) what makes most sense is to tell the host to update the parameters
            // as though a preset has been loaded.
            runOnMainThread([this] {
                if (isBeingDestroyed())
                    return;

                if (_host.canUseParams())
                    _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES);
            });
        }
#if JUCE_VERSION >= 0x060103
        if (details.nonParameterStateChanged)
        {
            runOnMainThread([this] {
                if (isBeingDestroyed())
                    return;

                if (_host.canUseState())
                    _host.stateMarkDirty();
            });
        }
#endif
        if (details.parameterInfoChanged)
        {
            // JUCE documentations states that, `parameterInfoChanged` means
            // "Indicates that some attributes of the AudioProcessor's parameters have changed."
            // For now, I'm going to assume this means the parameter's name or value->text
            // conversion has changed, and tell the clap host to rescan those.
            //
            // We could do CLAP_PARAM_RESCAN_ALL, but then the plugin would have to be deactivated.
            runOnMainThread([this] {
                if (isBeingDestroyed())
                    return;

                if (_host.canUseParams())
                    _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
            });
        }
    }
#else
    void audioProcessorChanged(juce::AudioProcessor *proc) override
    {
        /*
         * Before 6.0.8 it was unclear what changed. For now make the approximating decision to just
         * rescan values and text.
         */
        runOnMainThread([this] {
            if (isBeingDestroyed())
                return;

            if (_host.canUseParams())
                _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        });
    }
#endif

    clap_id clapIdFromParameterIndex(int index)
    {
        auto pbi = juceParameters.getParamForIndex(index);
        auto pf = clapIDByParamPtr.find(pbi);
        if (pf != clapIDByParamPtr.end())
            return pf->second;

        auto id = generateClapIDForJuceParam(pbi); // a lookup obviously
        return id;
    }

    std::atomic<bool> supressParameterChangeMessages{false};
    void audioProcessorParameterChanged(juce::AudioProcessor *, int index, float newValue) override
    {
        if (cacheHostCanUseThreadCheck)
        {
            /*
             * In this event the host can tell us the audio thread and so we can supress this
             * outbound message which would have resulted from the parameter change in the ::process
             * loop
             */
            if (_host.isAudioThread())
                return;
        }
        else
        {
            /*
             * In this case the host can't give us thread identities. To make sure we don't double
             * send an event, use an atomic bool to send the result. This may in a very rare
             * condition drop a UI event but will avoid a feedback cycle from the UI
             */
            if (supressParameterChangeMessages)
                return;
        }
        auto id = clapIdFromParameterIndex(index);
        uiParamChangeQ.push({CLAP_EVENT_PARAM_VALUE, 0, id, newValue});
    }

    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor *, int index) override
    {
        auto id = clapIdFromParameterIndex(index);
        auto p = paramPtrByClapID[id];
        uiParamChangeQ.push({CLAP_EVENT_PARAM_GESTURE_BEGIN, 0, id, p->getValue()});
    }

    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor *, int index) override
    {
        auto id = clapIdFromParameterIndex(index);
        auto p = paramPtrByClapID[id];
        uiParamChangeQ.push({CLAP_EVENT_PARAM_GESTURE_END, 0, id, p->getValue()});
    }

#if JUCE_VERSION < 0x070000
    /*
     * According to the JUCE docs this is *only* called on the processing thread
     */
    bool getCurrentPosition(juce::AudioPlayHead::CurrentPositionInfo &info) override
    {
        if (hasTransportInfo && transportInfo)
        {
            auto flags = transportInfo->flags;

            if (flags & CLAP_TRANSPORT_HAS_TEMPO)
                info.bpm = transportInfo->tempo;
            if (flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE)
            {
                info.timeSigNumerator = transportInfo->tsig_num;
                info.timeSigDenominator = transportInfo->tsig_denom;
            }

            if (flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
            {
                info.ppqPosition =
                    1.0 * (double)transportInfo->song_pos_beats / CLAP_BEATTIME_FACTOR;
                info.ppqPositionOfLastBarStart =
                    1.0 * (double)transportInfo->bar_start / CLAP_BEATTIME_FACTOR;
            }
            if (flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
            {
                info.timeInSeconds =
                    1.0 * (double)transportInfo->song_pos_seconds / CLAP_SECTIME_FACTOR;
                info.timeInSamples = (int64_t)(info.timeInSeconds * sampleRate());
            }
            info.isPlaying = flags & CLAP_TRANSPORT_IS_PLAYING;
            info.isRecording = flags & CLAP_TRANSPORT_IS_RECORDING;
            info.isLooping = flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE;
        }
        return hasTransportInfo;
    }
#else
    juce::Optional<PositionInfo> getPosition() const override
    {
        if (hasTransportInfo && transportInfo)
        {
            auto flags = transportInfo->flags;
            auto posinfo = PositionInfo();

            if (flags & CLAP_TRANSPORT_HAS_TEMPO)
                posinfo.setBpm(transportInfo->tempo);
            if (flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE)
            {
                auto ts = TimeSignature();
                ts.numerator = transportInfo->tsig_num;
                ts.denominator = transportInfo->tsig_denom;
                posinfo.setTimeSignature(ts);
            }

            if (flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
            {
                posinfo.setBarCount(transportInfo->bar_number);
                posinfo.setPpqPosition(1.0 * (double)transportInfo->song_pos_beats /
                                       CLAP_BEATTIME_FACTOR);
                posinfo.setPpqPositionOfLastBarStart(1.0 * (double)transportInfo->bar_start /
                                                     CLAP_BEATTIME_FACTOR);
            }
            if (flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
            {
                auto timeInSeconds =
                    1.0 * (double)transportInfo->song_pos_seconds / CLAP_SECTIME_FACTOR;
                posinfo.setTimeInSeconds(timeInSeconds);
                posinfo.setTimeInSamples((int64_t)(timeInSeconds * sampleRate()));
            }
            posinfo.setIsPlaying(flags & CLAP_TRANSPORT_IS_PLAYING);
            posinfo.setIsRecording(flags & CLAP_TRANSPORT_IS_RECORDING);
            posinfo.setIsLooping(flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE);

            return posinfo;
        }
        else
        {
            return juce::Optional<PositionInfo>();
        }
    }
#endif

    void parameterValueChanged(int, float newValue) override
    {
        juce::ignoreUnused(newValue);
        FIXME("parameter value changed");
        // this can only come from the bypass parameter
    }

    void parameterGestureChanged(int, bool) override { FIXME("parameter gesture changed"); }

    bool cacheHostCanUseThreadCheck{false};
    bool activate(double sampleRate, uint32_t minFrameCount,
                  uint32_t maxFrameCount) noexcept override
    {
        juce::ignoreUnused(minFrameCount);
        processor->setRateAndBufferSizeDetails(sampleRate, (int)maxFrameCount);
        processor->prepareToPlay(sampleRate, (int)maxFrameCount);
        midiBuffer.ensureSize(2048);
        midiBuffer.clear();

        cacheHostCanUseThreadCheck = _host.canUseThreadCheck();
        if (!cacheHostCanUseThreadCheck)
        {
            DBG("Host cannot support thread check. Using atomic guard for param feedback.");
        }

        return true;
    }

    void deactivate() noexcept override { processor->releaseResources(); }

    /* CLAP API */

    void defineAudioPorts()
    {
        jassert(!isActive());

        auto requested = processor->getBusesLayout();
        for (auto isInput : {true, false})
        {
            for (int i = 0; i < processor->getBusCount(isInput); ++i)
            {
                auto *bus = processor->getBus(isInput, i);
                auto busDefaultLayout = bus->getDefaultLayout();

                requested.getChannelSet(isInput, i) = busDefaultLayout;
            }
        }

        const auto success = processor->setBusesLayout(requested);
        jassert(success); // failed to set default bus layout!
        juce::ignoreUnused(success);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override
    {
        return (uint32_t)processor->getBusCount(isInput);
    }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info *info) const noexcept override
    {
        // For now hardcode to stereo out. Fix this obviously.
        const auto bus = processor->getBus(isInput, (int)index);
        const auto busDefaultLayout = bus->getDefaultLayout();

        // For now we only support mono or stereo buses
        jassert(busDefaultLayout == juce::AudioChannelSet::mono() ||
                busDefaultLayout == juce::AudioChannelSet::stereo());

        auto getPortID = [](bool isPortInput, uint32_t portIndex) {
            return (isPortInput ? 1 << 15 : 1) + portIndex;
        };

        info->id = getPortID(isInput, index);
        strncpy(info->name, bus->getName().toRawUTF8(), sizeof(info->name));

        bool couldBeMain = true;
        if (isInput && processorAsClapExtensions)
            couldBeMain = processorAsClapExtensions->isInputMain((int)index);
        if (index == 0 && couldBeMain)
        {
            info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        }
        else
        {
            info->flags = 0;
        }

        if (processor->getBus(!isInput, (int)index) != nullptr)
        {
            // this bus has a corresponding bus on the other side, so it can do in-place processing
            info->in_place_pair = getPortID(!isInput, index);
        }
        else
        {
            // this bus has no corresponding bus, so it can't do in-place processing
            info->in_place_pair = CLAP_INVALID_ID;
        }

        info->channel_count = (uint32_t)busDefaultLayout.size();

        if (busDefaultLayout == juce::AudioChannelSet::mono())
            info->port_type = CLAP_PORT_MONO;
        else if (busDefaultLayout == juce::AudioChannelSet::stereo())
            info->port_type = CLAP_PORT_STEREO;
        else
            jassertfalse; // @TODO: implement CLAP_PORT_SURROUND and CLAP_PORT_AMBISONIC through
                          // extensions

        return true;
    }
    uint32_t audioPortsConfigCount() const noexcept override
    {
        DBG("audioPortsConfigCount CALLED - returning 0");
        return 0;
    }
    bool audioPortsGetConfig(uint32_t /*index*/,
                             clap_audio_ports_config * /*config*/) const noexcept override
    {
        return false;
    }
    bool audioPortsSetConfig(clap_id /*configId*/) noexcept override { return false; }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool is_input) const noexcept override
    {
        if (is_input)
        {
            if (processor->acceptsMidi())
                return 1;
        }
        else
        {
            if (processor->producesMidi())
                return 1;
        }
        return 0;
    }
    bool notePortsInfo(uint32_t index, bool is_input,
                       clap_note_port_info *info) const noexcept override
    {
        juce::ignoreUnused(index);

        if (is_input)
        {
            info->id = 1 << 5U;
            info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
            if (processor->supportsMPE())
                info->supported_dialects |= CLAP_NOTE_DIALECT_MIDI_MPE;

            info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;

            if (processorAsClapExtensions)
            {
                if (processorAsClapExtensions->supportsNoteDialectClap(true))
                {
                    info->supported_dialects |= CLAP_NOTE_DIALECT_CLAP;
                }
                if (processorAsClapExtensions->prefersNoteDialectClap(true))
                {
                    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
                }
            }
            strncpy(info->name, "JUCE Note Input", CLAP_NAME_SIZE);
        }
        else
        {
            info->id = 1 << 2U;
            info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
            if (processor->supportsMPE())
                info->supported_dialects |= CLAP_NOTE_DIALECT_MIDI_MPE;
            info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;

            if (processorAsClapExtensions)
            {
                if (processorAsClapExtensions->supportsNoteDialectClap(false))
                {
                    info->supported_dialects |= CLAP_NOTE_DIALECT_CLAP;
                }
                if (processorAsClapExtensions->prefersNoteDialectClap(false))
                {
                    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
                }
            }

            strncpy(info->name, "JUCE Note Output", CLAP_NAME_SIZE);
        }
        return true;
    }

    bool implementsVoiceInfo() const noexcept override
    {
        if (processorAsClapExtensions)
            return processorAsClapExtensions->supportsVoiceInfo();
        return false;
    }

    bool voiceInfoGet(clap_voice_info *info) noexcept override
    {
        if (processorAsClapExtensions)
            return processorAsClapExtensions->voiceInfoGet(info);
        return Plugin::voiceInfoGet(info);
    }

  public:
    bool implementsParams() const noexcept override { return true; }
    bool isValidParamId(clap_id paramId) const noexcept override
    {
        return allClapIDs.find(paramId) != allClapIDs.end();
    }
    uint32_t paramsCount() const noexcept override { return (uint32_t)allClapIDs.size(); }
    bool paramsInfo(uint32_t paramIndex, clap_param_info *info) const noexcept override
    {
        auto pbi = juceParameters.getParamForIndex((int)paramIndex);

        auto *parameterGroup = processor->getParameterTree().getGroupsForParameter(pbi).getLast();
        juce::String group = "";
        while (parameterGroup && parameterGroup->getParent() &&
               parameterGroup->getParent()->getName().isNotEmpty())
        {
            group = parameterGroup->getName() + "/" + group;
            parameterGroup = parameterGroup->getParent();
        }

        if (group.isNotEmpty())
            group = "/" + group;

        // Fixme - using parameter groups here would be lovely but until then
        info->id = generateClapIDForJuceParam(pbi);
        strncpy(info->name, (pbi->getName(CLAP_NAME_SIZE)).toRawUTF8(), CLAP_NAME_SIZE);
        strncpy(info->module, group.toRawUTF8(), CLAP_NAME_SIZE);

        info->min_value = 0; // FIXME
        info->max_value = 1;
        info->default_value = pbi->getDefaultValue();
        info->cookie = pbi;
        info->flags = 0;

        if (pbi->isAutomatable())
            info->flags = info->flags | CLAP_PARAM_IS_AUTOMATABLE;

        if (pbi->isBoolean() || pbi->isDiscrete())
        {
            info->flags = info->flags | CLAP_PARAM_IS_STEPPED;
        }

        auto cpc = dynamic_cast<clap_juce_extensions::clap_juce_parameter_capabilities *>(pbi);
        if (cpc)
        {
            if (cpc->supportsMonophonicModulation())
            {
                info->flags = info->flags | CLAP_PARAM_IS_MODULATABLE;
            }
            if (cpc->supportsPolyphonicModulation())
            {
                info->flags =
                    info->flags | CLAP_PARAM_IS_MODULATABLE |
                    CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
                    CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID | CLAP_PARAM_IS_AUTOMATABLE_PER_PORT;
            }
        }

        return true;
    }

    bool paramsValue(clap_id paramId, double *value) noexcept override
    {
        auto pbi = paramPtrByClapID[paramId];
        *value = pbi->getValue();
        return true;
    }

    bool paramsValueToText(clap_id paramId, double value, char *display,
                           uint32_t size) noexcept override
    {
        auto pbi = paramPtrByClapID[paramId];
        if (!usingLegacyParameterAPI)
        {
            auto res = pbi->getText((float)value, (int)size);
            strncpy(display, res.toStdString().c_str(), size);
        }
        else
        {
            /*
             * This is really unsatisfactory but we have very little choice in the
             * event that the JUCE parameter mode is more or less like a VST2
             */
            auto res = pbi->getCurrentValueAsText();
            strncpy(display, res.toStdString().c_str(), size);
        }

        return true;
    }

    bool paramsTextToValue(clap_id paramId, const char *display, double *value) noexcept override
    {
        auto pbi = paramPtrByClapID[paramId];
        *value = pbi->getValueForText(display);
        return true;
    }

    void paramSetValueAndNotifyIfChanged(juce::AudioProcessorParameter &param, float newValue)
    {
        if (param.getValue() == newValue)
            return;

        param.setValueNotifyingHost(newValue);
    }

    bool implementsLatency() const noexcept override { return true; }
    uint32_t latencyGet() const noexcept override
    {
        return (uint32_t)processor->getLatencySamples();
    }

    bool implementsTail() const noexcept override { return true; }
    uint32_t tailGet(const clap_plugin_t *) const noexcept override
    {
        return uint32_t(
            juce::roundToIntAccurate((double)sampleRate() * processor->getTailLengthSeconds()));
    }

    juce::MidiBuffer midiBuffer;

    clap_process_status process(const clap_process *process) noexcept override
    {
        // Since the playhead is *only* good inside juce audio processor process,
        // we can just keep this little transient pointer here
        if (process->transport)
        {
            hasTransportInfo = true;
            transportInfo = process->transport;
        }
        else
        {
            hasTransportInfo = false;
            transportInfo = nullptr;
        }

        if (processorAsClapProperties)
            processorAsClapProperties->clap_transport = process->transport;

        auto pc = ParamChange();
        auto ov = process->out_events;

        while (uiParamChangeQ.pop(pc))
        {
            if (pc.type == CLAP_EVENT_PARAM_VALUE)
            {
                auto evt = clap_event_param_value();
                evt.header.size = sizeof(clap_event_param_value);
                evt.header.type = (uint16_t)CLAP_EVENT_PARAM_VALUE;
                evt.header.time = 0; // for now
                evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                evt.header.flags = (uint32_t)pc.flag;
                evt.param_id = pc.id;
                evt.value = pc.newval;
                ov->try_push(ov, reinterpret_cast<const clap_event_header *>(&evt));
            }

            if (pc.type == CLAP_EVENT_PARAM_GESTURE_END ||
                pc.type == CLAP_EVENT_PARAM_GESTURE_BEGIN)
            {
                auto evt = clap_event_param_gesture();
                evt.header.size = sizeof(clap_event_param_gesture);
                evt.header.type = (uint16_t)pc.type;
                evt.header.time = 0; // for now
                evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                evt.header.flags = (uint32_t)pc.flag;
                evt.param_id = pc.id;
                ov->try_push(ov, reinterpret_cast<const clap_event_header *>(&evt));
            }
        }

        if (processorAsClapExtensions && processorAsClapExtensions->supportsDirectProcess())
            return processorAsClapExtensions->clap_direct_process(process);

        auto ev = process->in_events;
        auto sz = ev->size(ev);

        if (sz != 0)
        {
            for (uint32_t i = 0; i < sz; ++i)
            {
                auto evt = ev->get(ev, i);

                if (evt->space_id != CLAP_CORE_EVENT_SPACE_ID)
                    continue;

                switch (evt->type)
                {
                case CLAP_EVENT_NOTE_ON:
                {
                    auto nevt = reinterpret_cast<const clap_event_note *>(evt);

                    midiBuffer.addEvent(juce::MidiMessage::noteOn(nevt->channel + 1, nevt->key,
                                                                  (float)nevt->velocity),
                                        (int)nevt->header.time);
                }
                break;
                case CLAP_EVENT_NOTE_OFF:
                {
                    auto nevt = reinterpret_cast<const clap_event_note *>(evt);
                    midiBuffer.addEvent(juce::MidiMessage::noteOff(nevt->channel + 1, nevt->key,
                                                                   (float)nevt->velocity),
                                        (int)nevt->header.time);
                }
                break;
                case CLAP_EVENT_MIDI:
                {
                    auto mevt = reinterpret_cast<const clap_event_midi *>(evt);
                    midiBuffer.addEvent(juce::MidiMessage(mevt->data[0], mevt->data[1],
                                                          mevt->data[2], mevt->header.time),
                                        (int)mevt->header.time);
                }
                break;
                case CLAP_EVENT_TRANSPORT:
                {
                    // handle this case
                }
                break;
                case CLAP_EVENT_PARAM_VALUE:
                {
                    auto pevt = reinterpret_cast<const clap_event_param_value *>(evt);

                    auto nf = pevt->value;
                    jassert(pevt->cookie == paramPtrByClapID[pevt->param_id]);
                    auto jp = static_cast<juce::AudioProcessorParameter *>(pevt->cookie);

                    /*
                     * In the event that a param value comes in from the host, we don't want
                     * to send it back out as a UI message but we do want to trigger any *other*
                     * listeners which may be attached. So suppress my listeners while we send this
                     * event.
                     */
                    auto g = AtomicTGuard<bool>(supressParameterChangeMessages, true);
                    paramSetValueAndNotifyIfChanged(*jp, (float)nf);
                }
                break;
                case CLAP_EVENT_PARAM_MOD:
                {
                }
                break;
                case CLAP_EVENT_NOTE_END:
                {
                    // Why do you send me this, Alex?
                }
                break;
                default:
                {
                    DBG("Unknown message type " << (int)evt->type);
                    // In theory I should never get this.
                    // jassertfalse
                }
                break;
                }
            }
        }

        // We process in place so
        static constexpr uint32_t maxBuses = 128;
        std::array<float *, maxBuses> busses{};
        busses.fill(nullptr);

        /*DBG("IO Configuration: I=" << (int)process->audio_inputs_count << " O="
                                   << (int)process->audio_outputs_count << " MX=" << (int)mx);
        DBG("Plugin Configuration: IC=" << processor->getTotalNumInputChannels()
                                        << " OC=" << processor->getTotalNumOutputChannels());
        */

        /*
         * OK so here is what JUCE expects in its audio buffer. It *always* uses input as output
         * buffer so we need to create a buffer where each channel is the channel of the associated
         * output pointer (fine) and then the inputs need to either check they are the same or copy.
         */

        /*
         * So first lets load up with our outputs
         */
        uint32_t ochans = 0;
        for (uint32_t idx = 0; idx < process->audio_outputs_count && ochans < maxBuses; ++idx)
        {
            for (uint32_t ch = 0; ch < process->audio_outputs[idx].channel_count; ++ch)
            {
                busses[ochans] = process->audio_outputs[idx].data32[ch];
                ochans++;
            }
        }

        uint32_t ichans = 0;
        for (uint32_t idx = 0; idx < process->audio_inputs_count && ichans < maxBuses; ++idx)
        {
            for (uint32_t ch = 0; ch < process->audio_inputs[idx].channel_count; ++ch)
            {
                auto *ic = process->audio_inputs[idx].data32[ch];
                if (ichans < ochans)
                {
                    if (ic == busses[ichans])
                    {
                        // The buffers overlap - no need to do anything
                    }
                    else
                    {
                        juce::FloatVectorOperations::copy(busses[ichans], ic,
                                                          (int)process->frames_count);
                    }
                }
                else
                {
                    busses[ichans] = ic;
                }
                ichans++;
            }
        }

        auto totalChans = std::max(ichans, ochans);
        juce::AudioBuffer<float> buf(busses.data(), (int)totalChans, (int)process->frames_count);

        FIXME("Handle bypass and deactivated states");
        processor->processBlock(buf, midiBuffer);

        if (processor->producesMidi())
        {
            for (auto meta : midiBuffer)
            {
                auto msg = meta.getMessage();
                if (msg.getRawDataSize() == 3)
                {
                    auto evt = clap_event_midi();
                    evt.header.size = sizeof(clap_event_midi);
                    evt.header.type = (uint16_t)CLAP_EVENT_MIDI;
                    evt.header.time = (uint32_t)meta.samplePosition; // for now
                    evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    evt.header.flags = 0;
                    evt.port_index = 0;
                    memcpy(&evt.data, msg.getRawData(), 3 * sizeof(uint8_t));
                    ov->try_push(ov, reinterpret_cast<const clap_event_header *>(&evt));
                }
            }
        }

        if (!midiBuffer.isEmpty())
            midiBuffer.clear();

        return CLAP_PROCESS_CONTINUE;
    }

    void reset() noexcept override
    {
        if (processorAsClapExtensions && processorAsClapExtensions->supportsDirectReset())
            return processorAsClapExtensions->clap_direct_reset();
    }

    void componentMovedOrResized(juce::Component &component, bool wasMoved,
                                 bool wasResized) override
    {
        juce::ignoreUnused(wasMoved);
        if (wasResized && _host.canUseGui())
            _host.guiRequestResize((uint32_t)component.getWidth(), (uint32_t)component.getHeight());
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    bool implementsGui() const noexcept override { return processor->hasEditor(); }
    bool guiCanResize() const noexcept override
    {
        if (editor)
            return editor->isResizable();
        return true;
    }

    /*
     * guiAdjustSize is called before guiSetSize and given the option to
     * reset the size the host hands to the subsequent setSize. This is a
     * relatively naive and unsatisfactory initial implementation.
     */
    bool guiAdjustSize(uint32_t *w, uint32_t *h) noexcept override
    {
        if (!editor)
            return false;

        if (!editor->isResizable())
            return false;

        auto cst = editor->getConstrainer();

        if (!cst)
            return true; // we have no constraints. Whaever is fine!

        auto minW = (uint32_t)cst->getMinimumWidth();
        auto maxW = (uint32_t)cst->getMaximumWidth();
        auto minH = (uint32_t)cst->getMinimumHeight();
        auto maxH = (uint32_t)cst->getMaximumHeight();

        // There is no std::clamp in c++14
        auto width = juce::jlimit(minW, maxW, *w);
        auto height = juce::jlimit(minH, maxH, *h);

        auto aspectRatio = (float)cst->getFixedAspectRatio();

        if (aspectRatio != 0.0)
        {
            /*
             * This is obviously an unsatisfactory algorithm, but we wanted to have
             * something at least workable here.
             *
             * The problem with other algorithms I tried is that this function gets
             * called by BWS for sub-single pixel motions on macOS, so it is hard to make
             * a version which is *stable* (namely adjust(w,h); cw=w;ch=h; adjust(cw,ch);
             * cw == w; ch == h) that deals with directions. I tried all sorts of stuff
             * and then ran into vacation.
             *
             * So for now here's this approach. See the discussion in CJE PR #67
             * and interop-tracker issue #30.
             */
            width = std::round(aspectRatio * height);
        }

        *w = width;
        *h = height;

        return true;
    }

    bool guiSetSize(uint32_t width, uint32_t height) noexcept override
    {
        if (!editor)
            return false;

        if (!editor->isResizable())
            return false;

        editor->setSize(static_cast<int>(width), static_cast<int>(height));
        return true;
    }

    bool guiIsApiSupported(const char *api, bool isFloating) noexcept override
    {
        if (isFloating)
            return false;

        if (strcmp(api, CLAP_WINDOW_API_WIN32) == 0 || strcmp(api, CLAP_WINDOW_API_COCOA) == 0 ||
            strcmp(api, CLAP_WINDOW_API_X11) == 0)
            return true;

        return false;
    }

    bool guiParentAttached{false};
    bool guiCreate(const char *api, bool isFloating) noexcept override
    {
        juce::ignoreUnused(api);

        // Should never happen
        if (isFloating)
            return false;

        const juce::MessageManagerLock mmLock;
        editor.reset(processor->createEditorIfNeeded());
        editor->addComponentListener(this);
        return editor != nullptr;
    }

    void guiDestroy() noexcept override
    {
        processor->editorBeingDeleted(editor.get());
        guiParentAttached = false;
        editor.reset(nullptr);
    }

    bool guiSetParent(const clap_window *window) noexcept override
    {
        guiParentAttached = true;
#if JUCE_MAC
        return guiCocoaAttach(window->cocoa);
#elif JUCE_LINUX
        return guiX11Attach(nullptr, window->x11);
#elif JUCE_WINDOWS
        return guiWin32Attach(window->win32);
#else
        guiParentAttached = false;
        return false;
#endif
    }

    // Show doesn't really exist in JUCE per se. If there's an editor and its attached
    // we are good.
    bool guiShow() noexcept override
    {
#if JUCE_MAC || JUCE_LINUX || JUCE_WINDOWS
        if (editor)
        {
            return guiParentAttached;
        }
#endif
        return false;
    }

    bool guiGetSize(uint32_t *width, uint32_t *height) noexcept override
    {
        const juce::MessageManagerLock mmLock;
        if (editor)
        {
            auto b = editor->getBounds();
            *width = (uint32_t)b.getWidth();
            *height = (uint32_t)b.getHeight();
            return true;
        }
        else
        {
            *width = 1000;
            *height = 800;
        }
        return false;
    }

  protected:
    juce::CriticalSection stateInformationLock;
    juce::MemoryBlock chunkMemory;

  public:
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream *stream) noexcept override
    {
        if (processor == nullptr)
            return false;

        juce::ScopedLock lock(stateInformationLock);
        chunkMemory.reset();

        processor->getStateInformation(chunkMemory);

        auto written = stream->write(stream, chunkMemory.getData(), chunkMemory.getSize());
        return written == (int64_t)chunkMemory.getSize();
    }
    bool stateLoad(const clap_istream *stream) noexcept override
    {
        if (processor == nullptr)
            return false;

        juce::ScopedLock lock(stateInformationLock);
        chunkMemory.reset();
        // There must be a better way
        char block[256];
        int64_t rd;
        while ((rd = stream->read(stream, block, 256)) > 0)
            chunkMemory.append(block, (size_t)rd);

        processor->setStateInformation(chunkMemory.getData(), (int)chunkMemory.getSize());
        chunkMemory.reset();
        return true;
    }

  public:
#if JUCE_MAC
    bool guiCocoaAttach(void *nsView) noexcept
    {
        juce::initialiseMacVST();
        auto hostWindow = juce::attachComponentToWindowRefVST(editor.get(), nsView, true);
        juce::ignoreUnused(hostWindow);
        return true;
    }
#endif

#if JUCE_LINUX
    bool guiX11Attach(const char *displayName, unsigned long window) noexcept
    {
        juce::ignoreUnused(displayName);
        const juce::MessageManagerLock mmLock;
        editor->setVisible(false);
        editor->addToDesktop(0, (void *)window);
        auto *display = juce::XWindowSystem::getInstance()->getDisplay();
        juce::X11Symbols::getInstance()->xReparentWindow(display, (Window)editor->getWindowHandle(),
                                                         window, 0, 0);
        editor->setVisible(true);
        return true;
    }
#endif

#if JUCE_WINDOWS
    bool guiWin32Attach(clap_hwnd window) noexcept
    {
        editor->setVisible(false);
        editor->setOpaque(true);
        editor->setTopLeftPosition(0, 0);
        editor->addToDesktop(0, (void *)window);
        editor->setVisible(true);
        return true;
    }
#endif

  private:
    struct ParamChange
    {
        int type;
        int flag;
        uint32_t id;
        float newval{0};
    };
    PushPopQ<ParamChange, 4096 * 16> uiParamChangeQ;

    /*
     * Various maps for ID lookups
     */
    // clap_id to param *
    std::unordered_map<clap_id, juce::AudioProcessorParameter *> paramPtrByClapID;
    // param * to clap_id
    std::unordered_map<juce::AudioProcessorParameter *, clap_id> clapIDByParamPtr;
    // Every id we have issued
    std::unordered_set<clap_id> allClapIDs;

    juce::LegacyAudioParametersWrapper juceParameters;

    const clap_event_transport *transportInfo{nullptr};
    bool hasTransportInfo{false};
};

JUCE_END_IGNORE_WARNINGS_MSVC

const char *features[] = {CLAP_FEATURES, nullptr};
clap_plugin_descriptor ClapJuceWrapper::desc = {CLAP_VERSION,
                                                CLAP_ID,
                                                JucePlugin_Name,
                                                JucePlugin_Manufacturer,
                                                JucePlugin_ManufacturerWebsite,
                                                CLAP_MANUAL_URL,
                                                CLAP_SUPPORT_URL,
                                                JucePlugin_VersionString,
                                                JucePlugin_Desc,
                                                features};

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wredundant-decls")
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter();
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace ClapAdapter
{
static bool clap_init(const char *) { return true; }

static void clap_deinit(void) {}

static uint32_t clap_get_plugin_count(const struct clap_plugin_factory *) { return 1; }

static const clap_plugin_descriptor *clap_get_plugin_descriptor(const struct clap_plugin_factory *,
                                                                uint32_t)
{
    return &ClapJuceWrapper::desc;
}

static const clap_plugin *clap_create_plugin(const struct clap_plugin_factory *,
                                             const clap_host *host, const char *plugin_id)
{
    juce::ScopedJuceInitialiser_GUI libraryInitialiser;

    if (strcmp(plugin_id, ClapJuceWrapper::desc.id))
    {
        std::cout << "Warning: CLAP asked for plugin_id '" << plugin_id
                  << "' and JuceCLAPWrapper ID is '" << ClapJuceWrapper::desc.id << "'"
                  << std::endl;
        return nullptr;
    }
    clap_juce_extensions::clap_properties::building_clap = true;
    clap_juce_extensions::clap_properties::clap_version_major = CLAP_VERSION_MAJOR;
    clap_juce_extensions::clap_properties::clap_version_minor = CLAP_VERSION_MINOR;
    clap_juce_extensions::clap_properties::clap_version_revision = CLAP_VERSION_REVISION;
    auto *const pluginInstance = ::createPluginFilter();
    clap_juce_extensions::clap_properties::building_clap = false;
    auto *wrapper = new ClapJuceWrapper(host, pluginInstance);
    return wrapper->clapPlugin();
}

static const struct clap_plugin_factory juce_clap_plugin_factory = {
    ClapAdapter::clap_get_plugin_count,
    ClapAdapter::clap_get_plugin_descriptor,
    ClapAdapter::clap_create_plugin,
};

static const void *clap_get_factory(const char *factory_id)
{
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
    {
        return &juce_clap_plugin_factory;
    }

    return nullptr;
}

} // namespace ClapAdapter

extern "C"
{
#if JUCE_LINUX
#pragma GCC diagnostic ignored "-Wattributes"
#endif
    const CLAP_EXPORT struct clap_plugin_entry clap_entry = {CLAP_VERSION, ClapAdapter::clap_init,
                                                             ClapAdapter::clap_deinit,
                                                             ClapAdapter::clap_get_factory};
}
