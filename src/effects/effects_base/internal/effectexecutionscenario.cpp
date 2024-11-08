/*
* Audacity: A Digital Audio Editor
*/
#include "effectexecutionscenario.h"

#include "global/realfn.h"

#include "libraries/lib-project/Project.h"
//#include "libraries/lib-audacity-application-logic/AudacityApplicationLogic.h"
#include "libraries/lib-effects/Effect.h"
#include "libraries/lib-module-manager/PluginManager.h"

#include "libraries/lib-track/Track.h"
#include "libraries/lib-wave-track/WaveTrack.h"
#include "libraries/lib-project-rate/ProjectRate.h"
#include "libraries/lib-time-frequency-selection/ViewInfo.h"
#include "libraries/lib-menus/CommandManager.h"
#include "libraries/lib-effects/EffectManager.h"
#include "libraries/lib-project-history/ProjectHistory.h"
#include "libraries/lib-module-manager/ConfigInterface.h"
#include "libraries/lib-numeric-formats/NumericConverterFormats.h"

#include "au3wrap/internal/wxtypes_convert.h"
#include "au3wrap/au3types.h"

using namespace muse;
using namespace au::effects;

static const int UNDEFINED_FREQUENCY = -1;

muse::Ret EffectExecutionScenario::performEffect(const EffectId& effectId)
{
    au3::Au3Project& project = projectRef();
    return doPerformEffect(project, effectId, 0);
}

au::au3::Au3Project& EffectExecutionScenario::projectRef()
{
    return *reinterpret_cast<au3::Au3Project*>(globalContext()->currentProject()->au3ProjectPtr());
}

muse::Ret EffectExecutionScenario::repeatLastProcessor()
{
    IF_ASSERT_FAILED(m_lastProcessorId) {
        return muse::make_ret(Ret::Code::UnknownError);
    }
    au3::Au3Project& project = projectRef();
    return doPerformEffect(project, *m_lastProcessorId, EffectManager::kConfigured);
}

muse::Ret EffectExecutionScenario::doPerformEffect(au3::Au3Project& project, const EffectId& effectId, unsigned flags)
{
    //! ============================================================================
    //! NOTE Step 1 - check input params (effect is present and available, selection)
    //! ============================================================================

    // common things used below
    PluginID ID = effectId.toStdString();
    EffectManager& em = EffectManager::Get();
    Effect* effect = nullptr;

    NotifyingSelectedRegion& selectedRegion = ViewInfo::Get(project).selectedRegion;
    bool isSelection = false;

    {
        //! NOTE Step 1.1 - check plugin
        const PluginDescriptor* plug = PluginManager::Get().GetPlugin(ID);
        if (!plug || !PluginManager::IsPluginAvailable(*plug)) {
            return muse::make_ret(Ret::Code::UnknownError);
        }

        //! NOTE Step 1.2 - get effect
        effect = dynamic_cast<Effect*>(em.GetEffect(ID));
        IF_ASSERT_FAILED(effect) {
            return muse::make_ret(Ret::Code::InternalError);
        }

        //! NOTE Step 1.3 - check selection

        IF_ASSERT_FAILED(muse::RealIsEqualOrMore(selectedRegion.duration(), 0.0)) {
            return muse::make_ret(Ret::Code::InternalError);
        }

        isSelection = selectedRegion.t1() > selectedRegion.t0();

        //! TODO Should we do something if there is no selection and the effect is not a generator? Maybe add a check... or automatically select all...

        // Make sure there's no activity since the effect is about to be applied
        // to the project's tracks.  Mainly for Apply during RTP, but also used
        // for batch commands
        if (flags & EffectManager::kConfigured) {
            //! TODO
            // DO stopPlayback;
        }
    }

    //! ============================================================================
    //! NOTE Step 2 - formation of settings
    //! ============================================================================

    // common things used below
    EffectSettings* settings = nullptr;
    EffectTimeParams tp;
    tp.projectRate = ProjectRate::Get(project).GetRate();

    double oldDuration = 0.0;
    {
        //! NOTE Step 2.1 - get effect settings
        settings = em.GetDefaultSettings(ID);
        IF_ASSERT_FAILED(settings) {
            return muse::make_ret(Ret::Code::InternalError);
        }

        //! NOTE Step 2.2 - get oldDuration for EffectTypeGenerate
        if (effect->GetType() == EffectTypeGenerate) {
            GetConfig(effect->GetDefinition(), PluginSettings::Private,
                      CurrentSettingsGroup(),
                      EffectSettingsExtra::DurationKey(), oldDuration, effect->GetDefaultDuration());
        }

        //! NOTE Step 2.3 - check selected time
        double duration = 0.0;
        tp.t0 = selectedRegion.t0();
        tp.t1 = selectedRegion.t1();
        if (tp.t1 > tp.t0) {
            // there is a selection: let's fit in there...
            // MJS: note that this is just for the TTC and is independent of the track rate
            // but we do need to make sure we have the right number of samples at the project rate
            double quantMT0 = QUANTIZED_TIME(tp.t0, tp.projectRate);
            double quantMT1 = QUANTIZED_TIME(tp.t1, tp.projectRate);
            duration = quantMT1 - quantMT0;
            tp.t1 = tp.t0 + duration;
        }

        tp.f0 = selectedRegion.f0();
        tp.f1 = selectedRegion.f1();

        //! NOTE Step 2.4 - update settings
        wxString newFormat = (isSelection
                              ? NumericConverterFormats::TimeAndSampleFormat()
                              : NumericConverterFormats::DefaultSelectionFormat()
                              ).Internal();

        settings->extra.SetDuration(duration);
        settings->extra.SetDurationFormat(newFormat);
    }

    //! ============================================================================
    //! NOTE Step 3 - setup effect
    //! (must be before creating an instance and initializing it)
    //! ============================================================================
    unsigned oldFlags = 0;
    {
        //! NOTE Step 1.1 - setup effect
        oldFlags = effect->mUIFlags;
        effect->mUIFlags = flags;
        effect->mFactory = &WaveTrackFactory::Get(project);
        effect->mProjectRate = tp.projectRate;
        effect->mT0 = tp.t0;
        effect->mT1 = tp.t1;

        effect->SetTracks(&au3::Au3TrackList::Get(project));
        // Update track/group counts
        effect->CountWaveTracks();

        //! NOTE Step 1.2 - check frequency params
        effect->mF0 = tp.f0;
        effect->mF1 = tp.f1;
        if (effect->mF0 != UNDEFINED_FREQUENCY) {
            effect->mPresetNames.push_back(L"control-f0");
        }
        if (effect->mF1 != UNDEFINED_FREQUENCY) {
            effect->mPresetNames.push_back(L"control-f1");
        }
    }

    //! ============================================================================
    //! NOTE Step 4 - Make and init instance
    //! ============================================================================
    std::shared_ptr<EffectInstanceEx> pInstanceEx;
    {
        pInstanceEx = std::dynamic_pointer_cast<EffectInstanceEx>(effect->MakeInstance());
        if (!pInstanceEx || !pInstanceEx->Init()) {
            return muse::make_ret(Ret::Code::InternalError);
        }
    }

    //! ============================================================================
    //! NOTE Step 5 - modify settings by user
    //! ============================================================================
    {
        if (effect->IsInteractive() && (flags& EffectManager::kConfigured) == 0) {
            muse::String type = au3::wxToString(effect->GetSymbol().Internal());
            EffectInstanceId instanceId = effectInstancesRegister()->regInstance(effect, settings);
            muse::Ret ret = effectsProvider()->showEffect(type, instanceId);
            effectInstancesRegister()->unregInstance(effect);
            if (ret) {
                effect->SaveUserPreset(CurrentSettingsGroup(), *settings);
            } else {
                LOGE() << "failed show effect: " << type << ", ret: " << ret.toString();
                return ret;
            }
        }

        //! TODO
        em.SetSkipStateFlag(false);
    }

    //! ============================================================================
    //! NOTE Step 6 - perform effect
    //! ============================================================================
    // common things used below
    Ret success;
    {
        success = effectsProvider()->performEffect(project, effect, pInstanceEx, *settings);
    }

    //! ============================================================================
    //! NOTE Step 7 - cleanup
    //! ============================================================================

    {
        //! NOTE Step 7.1 - cleanup effect
        // Don't hold a dangling pointer when done
        effect->SetTracks(nullptr);
        effect->mPresetNames.clear();
        effect->mUIFlags = oldFlags;

        //! NOTE Step 7.2 - update selected region after process

        //! TODO It is not clear, can the effect change the selected region?
        //! Or is it done because the selected region changes when
        //! setting the parameters (because of QUANTIZED_TIME)
        //! Should we notify the UI about the change of the selected region and show the user its change?
        if (success && (effect->mT1 >= effect->mT0)) {
            selectedRegion.setTimes(effect->mT0, effect->mT1);
        }

        //! NOTE Step 7.3 - cleanup
        if (!success) {
            // On failure, restore the old duration setting
            settings->extra.SetDuration(oldDuration);
        }
    }

    //! NOTE break if not success
    if (!success) {
        return muse::make_ret(Ret::Code::UnknownError);
    }

    //! ============================================================================
    //! NOTE Step 8 - write history
    //! ============================================================================

    {
        //! NOTE Step 8.1 - write project history if need
        if (em.GetSkipStateFlag()) {
            flags = flags | EffectManager::kSkipState;
        }

        if (!(flags & EffectManager::kSkipState)) {
            auto shortDesc = PluginManager::Get().GetName(ID);
            const auto longDesc = XO("Applied effect: %s").Format(shortDesc);
            ProjectHistory::Get(project).PushState(longDesc, shortDesc);
        }

        //! NOTE Step 8.2 - remember a successful effect
        if (!(flags & EffectManager::kDontRepeatLast) && effect->GetType() == EffectTypeProcess) {
            if (m_lastProcessorId != effectId) {
                const auto firstTime = !m_lastProcessorId.has_value();
                m_lastProcessorId = effectId;
                m_lastProcessorIdChanged.send(effectId);
                if (firstTime) {
                    m_lastProcessorIsAvailableChanged.notify();
                }
            }
        }
    }

    return true;
}

bool EffectExecutionScenario::lastProcessorIsAvailable() const
{
    return m_lastProcessorId.has_value();
}

muse::async::Notification EffectExecutionScenario::lastProcessorIsNowAvailable() const
{
    return m_lastProcessorIsAvailableChanged;
}

muse::async::Channel<EffectId> EffectExecutionScenario::lastProcessorIdChanged() const
{
    return m_lastProcessorIdChanged;
}

muse::Ret EffectExecutionScenario::previewEffect(const EffectInstanceId& effectInstanceId, EffectSettings& settings)
{
    au3::Au3Project& project = projectRef();
    Effect* effect = effectInstancesRegister()->instanceById(effectInstanceId);
    return effectsProvider()->previewEffect(project, effect, settings);
}
