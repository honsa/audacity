/*
* Audacity: A Digital Audio Editor
*/

#include "au3audiooutput.h"

#include "global/async/async.h"

#include "libraries/lib-audio-io/AudioIO.h"
#include "libraries/lib-audio-io/ProjectAudioIO.h"

#include "au3audioinoutmeter.h"
#include "au3types.h"

#include "playback/audiotypes.h"

#include "log.h"

using namespace muse;
using namespace muse::async;
using namespace au::au3;

Au3AudioOutput::Au3AudioOutput()
{
    m_outputMeter = std::make_shared<InOutMeter>();

    globalContext()->currentProjectChanged().onNotify(this, [this](){
        auto currentProject = globalContext()->currentProject();
        if (!currentProject) {
            return;
        }

        initMeter();
    });
}

void Au3AudioOutput::initMeter()
{
    AudacityProject& project = projectRef();

    auto& projectAudioIO = ProjectAudioIO::Get(project);
    projectAudioIO.SetPlaybackMeter(m_outputMeter);
}

muse::async::Promise<float> Au3AudioOutput::playbackVolume() const
{
    return muse::async::Promise<float>([](auto resolve, auto /*reject*/) {
        float inputVolume;
        float outputVolume;
        int inputSource;

        auto gAudioIO = AudioIO::Get();
        gAudioIO->GetMixer(&inputSource, &inputVolume, &outputVolume);

        return resolve(au3VolumeToLocal(outputVolume));
    });
}

void Au3AudioOutput::setPlaybackVolume(float volume)
{
    muse::async::Async::call(this, [this, volume]() {
        float inputVolume;
        float outputVolume;
        int inputSource;

        auto gAudioIO = AudioIO::Get();
        gAudioIO->GetMixer(&inputSource, &inputVolume, &outputVolume);

        gAudioIO->SetMixer(inputSource, inputVolume, localVolumeToAu3(volume));

        m_playbackVolumeChanged.send(volume);
    });
}

muse::async::Channel<float> Au3AudioOutput::playbackVolumeChanged() const
{
    return m_playbackVolumeChanged;
}

muse::async::Promise<muse::async::Channel<au::audio::audioch_t, au::audio::AudioSignalVal> > Au3AudioOutput::playbackSignalChanges() const
{
    return m_outputMeter->signalChanges();
}

AudacityProject& Au3AudioOutput::projectRef() const
{
    AudacityProject* project = reinterpret_cast<AudacityProject*>(globalContext()->currentProject()->au3ProjectPtr());
    return *project;
}
