/*
 * VST3 wrapper around the drum machine.
 *
 * The plugin is a single component: one object is both the audio processor and
 * the edit controller, and its editor is the machine's own interface (see
 * view_x11.cpp / view_win.cpp). The engine and the interface are the same C
 * code the standalone links, so a pattern made in the plugin is exactly the
 * pattern the app plays.
 *
 * The host owns the clock: the internal mixer runs only while the host
 * transport does, and stops with it.
 */
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <cstring>
#include <vector>

#include "audio.h"
#include "cdmvst.h"
#include "ui.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// {6E1F6A54-2C6B-4B8E-9B0B-3E2A4B7C1D10}
static const FUID kPluginUID (0x6E1F6A54, 0x2C6B4B8E, 0x9B0B3E2A, 0x4B7C1D10);
static const FUID kControllerUID (0x6E1F6A55, 0x2C6B4B8E, 0x9B0B3E2A, 0x4B7C1D10);

constexpr const char *kName = "Circular Drum Machine";
constexpr const char *kVendor = "Spyro 24";
constexpr const char *kUrl = "https://example.invalid/circular-drum-machine";
constexpr const char *kMail = "mailto:spyro24@example.invalid";
constexpr const char *kVersion = "0.1.0";

class Plugin : public SingleComponentEffect {
public:
    Plugin () = default;

    ~Plugin () override
    {
        free_host ();
    }

    static FUnknown *createInstance (void *)
    {
        return static_cast<IAudioProcessor *> (new Plugin ());
    }

    //--- IPluginBase -------------------------------------------------------
    tresult PLUGIN_API initialize (FUnknown *context) SMTG_OVERRIDE
    {
        tresult r = SingleComponentEffect::initialize (context);
        if (r != kResultOk)
            return r;

        addAudioOutput (USTRING ("Output"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API terminate () SMTG_OVERRIDE
    {
        free_host ();
        return SingleComponentEffect::terminate ();
    }

    //--- IComponent --------------------------------------------------------
    tresult PLUGIN_API setActive (TBool state) SMTG_OVERRIDE
    {
        if (state)
            make_host ();
        else
            mixer_play (h_.mixer, 0);
        return kResultOk;
    }

    //--- IAudioProcessor ---------------------------------------------------
    tresult PLUGIN_API setupProcessing (ProcessSetup &setup) SMTG_OVERRIDE
    {
        rate_ = setup.sampleRate > 0 ? setup.sampleRate : 48000.0;
        if (h_.mixer) {
            mixer_free (h_.mixer);
            h_.mixer = mixer_new ((int) (rate_ + 0.5));
            if (h_.ui)
                ui_set_mixer (h_.ui, h_.mixer);
        }
        return SingleComponentEffect::setupProcessing (setup);
    }

    tresult PLUGIN_API setProcessing (TBool /*state*/) SMTG_OVERRIDE { return kResultOk; }

    tresult PLUGIN_API process (ProcessData &data) SMTG_OVERRIDE
    {
        if (!h_.mixer || data.numSamples <= 0)
            return kResultOk;

        const ProcessContext *ctx = data.processContext;
        const int playing = ctx && (ctx->state & ProcessContext::kPlaying) ? 1 : 0;

        if (playing != playing_) {
            mixer_play (h_.mixer, playing);
            playing_ = playing;
        }

        // The tail of a sample keeps ringing after the host stops, which is
        // what an instrument should do; the pattern itself never advances.
        const int n = data.numSamples;
        scratch_.assign ((size_t) n * 2, 0.0f);
        mixer_render (h_.mixer, scratch_.data (), n);

        if (data.numOutputs > 0) {
            AudioBusBuffers &out = data.outputs[0];
            for (int ch = 0; ch < out.numChannels; ch++) {
                Sample32 *dst = out.channelBuffers32 ? out.channelBuffers32[ch] : nullptr;
                if (!dst)
                    continue;
                const float *src = scratch_.data () + (ch & 1);
                for (int i = 0; i < n; i++)
                    dst[i] = src[(size_t) i * 2];
            }
        }
        return kResultOk;
    }

    //--- IEditController ---------------------------------------------------
    IPlugView *PLUGIN_API createView (FIDString /*name*/) SMTG_OVERRIDE
    {
        if (!h_.ui)
            make_host ();
        return cdm::create_platform_view (&h_);
    }

private:
    void make_host ()
    {
        if (!h_.canvas)
            h_.canvas = canvas_new (CDM_VIEW_W, CDM_VIEW_H);
        if (!h_.ui)
            h_.ui = ui_create ();
        if (!h_.mixer)
            h_.mixer = mixer_new ((int) (rate_ + 0.5));
        if (h_.ui && h_.mixer)
            ui_set_mixer (h_.ui, h_.mixer);
        h_.plugin = this;
    }

    void free_host ()
    {
        // The editor may still be attached on another thread; the host
        // detaches it before terminating, so releasing here is safe.
        ui_destroy (h_.ui);
        canvas_free (h_.canvas);
        mixer_free (h_.mixer);
        h_.ui = nullptr;
        h_.canvas = nullptr;
        h_.mixer = nullptr;
    }

    cdm::host h_ {};
    std::vector<float> scratch_;
    double rate_ = 48000.0;
    int playing_ = 0;
};

} /* namespace */

//------------------------------------------------------------------------
BEGIN_FACTORY (kVendor, kUrl, kMail, PFactoryInfo::kNoFlags)

	// A single component: the same class is processor and controller.
	DEF_CLASS2 (INLINE_UID_FROM_FUID (kPluginUID),
	            PClassInfo::kManyInstances,
	            kVstAudioEffectClass,
	            kName,
	            Vst::kSimpleModeSupported,
	            PlugType::kInstrument,
	            kVersion,
	            kVstVersionString,
	            Plugin::createInstance)

END_FACTORY
