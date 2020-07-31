#include "playbackmanager.h"
#include <QMutexLocker>
#include <QDebug>
#include "RtAudio.h"

PlaybackManager::PlaybackManager(QObject* parent) :
    QObject(parent),
    m_activeSample(0),
    m_outChannels(2),
    m_audio(new RtAudio()),
    m_volume(1.0),
    m_fadeOutSamples(0),
    m_currFadeOut(0),
    m_fadeOutStarted(0),
    m_fadeInSamples(0),
    m_currFadeIn(0),
    m_deviceCacheDirty(true)
{

}

PlaybackManager::~PlaybackManager()
{
    close();
    delete m_audio;
}

QStringList PlaybackManager::pollDevices()
{
    m_deviceCacheDirty = true;
    return getDevices();
}

QStringList PlaybackManager::getDevices()
{
    if (m_deviceCacheDirty) {
        m_deviceCache.clear();
        for (uint i = 0; i < m_audio->getDeviceCount(); i++) {
            auto info = m_audio->getDeviceInfo(i);
            if (info.probed) {
                m_deviceCache.append(info);
            }
        }
        m_deviceCacheDirty = false;
    }
    QStringList devices;
    for (auto device : m_deviceCache) {
        devices.append(device.name.c_str());
    }
    return devices;
}

QString PlaybackManager::currentDevice() const
{
    return m_currentDevice;
}

int playbackCallback(void *outputBuffer, void *inputBuffer, unsigned int nFrames, double streamTime, RtAudioStreamStatus status, void *userData);
void playbackErrorCallback(RtAudioError::Type type, const std::string &errorText);

bool PlaybackManager::openDevice(const QString &deviceName)
{
    close();
    m_currentDevice = QString();

    int idx = getDevices().indexOf(deviceName);
    if (idx < 0) {
        reportError(QString("Requested audio device is not available: %1").arg(deviceName));
        return false;
    }
    RtAudio::DeviceInfo info = m_deviceCache.at(idx);

    RtAudio::StreamParameters parameters;
    parameters.deviceId = idx;
    // stereo main and aux
    if (info.outputChannels >= 4) {
        m_outChannels = 4;
    }
    // just stereo main
    else {
        m_outChannels = 2;
    }
    parameters.nChannels = m_outChannels;
    unsigned int sampleRate = 44100;
    unsigned int bufferFrames = 256;
    RtAudio::StreamOptions options;
    //options.flags = RTAUDIO_NONINTERLEAVED;

    try {
        m_audio->openStream( &parameters, NULL, RTAUDIO_FLOAT32,
                        sampleRate, &bufferFrames, &playbackCallback, this, &options );
        m_currentDevice = deviceName;
        emit opened();
        return true;
    }
    catch ( RtAudioError& e ) {
        reportError(QString("Error opening audio stream: %1").arg(e.getMessage().c_str()));
        return false;
    }
}

void PlaybackManager::close()
{
    if (m_audio->isStreamOpen()) {
        abort();
        m_audio->closeStream();
        emit closed();
    }
}

void PlaybackManager::start(int fadeInSamples)
{
    if (m_audio->isStreamOpen() && !m_audio->isStreamRunning()) {
        m_fadeInSamples = fadeInSamples;
        m_currFadeIn = 0;
        try {
            m_audio->startStream();
            emit started();
        }
        catch ( RtAudioError& e ) {
            reportError(QString("Error starting audio stream: %1").arg(e.getMessage().c_str()));
        }
    }
}

void PlaybackManager::stop(int fadeOutSamples)
{
    if (m_audio->isStreamOpen() && m_audio->isStreamRunning()) {
        if (m_fadeOutStarted == 0) {
            m_fadeOutStarted = 1;
            m_fadeOutSamples = fadeOutSamples;
            m_currFadeOut = 0;

            if (fadeOutSamples == 0) {
                m_fadeOutStarted = 0;
                m_audio->stopStream();
                emit stopped();
            }
        }
    }
}

void PlaybackManager::abort()
{
    if (m_audio->isStreamOpen() && m_audio->isStreamRunning()) {
        m_audio->abortStream();
        emit stopped();
    }
}

bool PlaybackManager::isRunning()
{
    return m_audio->isStreamRunning();
}

QSharedPointer<TrackData> PlaybackManager::queuedTrack() const
{
    return m_queuedTrack;
}

void PlaybackManager::setQueuedTrack(QSharedPointer<TrackData> queuedTrack, QSharedPointer<TrackData> auxTrack)
{
    m_mutex.lock();
    m_queuedTrack = queuedTrack;
    m_queuedAuxTrack = auxTrack;
    m_mutex.unlock();

    if (!m_queuedTrack.isNull() && m_queuedTrack->autoPlay()) {
        start();
    }

    emit queueChanged();
}

void PlaybackManager::clearActiveTrack()
{
    QMutexLocker lock(&m_mutex);
    m_activeTrack = QSharedPointer<TrackData>();
    m_activeAuxTrack = QSharedPointer<TrackData>();
    m_activeSample = 0;
    emit queueChanged();
}

QSharedPointer<TrackData> PlaybackManager::activeTrack() const
{
    return m_activeTrack;
}

int PlaybackManager::activeSample() const
{
    return m_activeSample;
}

void PlaybackManager::checkQueue()
{
    m_activeTrack = QSharedPointer<TrackData>();
    m_activeAuxTrack = QSharedPointer<TrackData>();
    if (!m_queuedTrack.isNull()) {
        m_activeTrack = m_queuedTrack;
        m_activeAuxTrack = m_queuedAuxTrack;
        m_activeSample = 0;
        m_queuedTrack = QSharedPointer<TrackData>();
        m_queuedAuxTrack = QSharedPointer<TrackData>();

        emit trackStarted(m_activeTrack->fileName());
    }
    emit queueChanged();
}

int PlaybackManager::outChannels() const
{
    return m_outChannels;
}

void PlaybackManager::setOutChannels(int outChannels)
{
    m_outChannels = outChannels;
}

bool PlaybackManager::trackOk(QSharedPointer<TrackData> track)
{
    return !track.isNull() && !track->buffer().isNull() && track->buffer()->floatData().size() > 512;
}

void PlaybackManager::setVolume(float volume)
{
    m_volume = volume;
}

int PlaybackManager::writeNextAudioData(void *outputBuffer, unsigned int nFrames)
{
    QMutexLocker lock(&m_mutex);
    float* out = static_cast<float*>(outputBuffer);

    if (m_activeTrack.isNull()) {
        checkQueue();
    }

    if (!trackOk(m_activeTrack)) {
        return endPlayback();
    }

    bool auxPlayback = m_outChannels == 4 && trackOk(m_activeAuxTrack);

    for (unsigned int i = 0; i < nFrames; i++) {
        int sample = m_activeSample;
        if (m_fadeOutStarted == 1) {
            sample += (m_currFadeOut * m_activeTrack->buffer()->numChannels());
        }
        if (sample + 1 > m_activeTrack->buffer()->floatData().size()) {
            m_activeSample = 0;
            sample = m_activeSample;
            checkQueue();
            if (!trackOk(m_activeTrack)) {
                return endPlayback();
            }
            else if (!m_activeTrack.isNull() && m_activeTrack->autoStop()) {
                return endPlayback();
            }
            auxPlayback = m_outChannels == 4 && trackOk(m_activeAuxTrack);
        }

        float volume = m_volume;
        if (m_currFadeIn < m_fadeInSamples) {
            m_currFadeIn++;
            volume *= float(m_currFadeIn * m_currFadeIn) / float(m_fadeInSamples * m_fadeInSamples);
        }
        if (m_fadeOutStarted == 1) {
            if (m_currFadeOut < m_fadeOutSamples) {
                m_currFadeOut++;
                float reduction = float(m_currFadeOut * m_currFadeOut) / float(m_fadeOutSamples * m_fadeOutSamples);
                volume *= (1.0f - reduction);
            }
            else {
                return endPlayback();
            }
        }

        out[i*m_outChannels] = m_activeTrack->buffer()->floatData().at(sample) * volume;
        if (m_activeTrack->buffer()->numChannels() < 2) {
            out[i*m_outChannels + 1] = out[i*m_outChannels];
        }
        else {
            out[i*m_outChannels + 1] = m_activeTrack->buffer()->floatData().at(sample+1) * volume;
        }
        // Aux output
        if (auxPlayback) {
            int auxSample = sample / m_activeTrack->buffer()->numChannels();
            auxSample *= m_activeAuxTrack->buffer()->numChannels();
            auxSample %= m_activeAuxTrack->buffer()->floatData().size();
            out[i*m_outChannels + 2] = m_activeAuxTrack->buffer()->floatData().at(auxSample) * volume;
            if (m_activeAuxTrack->buffer()->numChannels() < 2) {
                out[i*m_outChannels + 3] = out[i * m_outChannels + 2];
            }
            else {
                out[i*m_outChannels + 3] = m_activeAuxTrack->buffer()->floatData().at(auxSample + 1) * volume;
            }
        }

        if (m_fadeOutStarted != 1) {
            m_activeSample += m_activeTrack->buffer()->numChannels();
        }
    }

    return 0;
}

int PlaybackManager::endPlayback()
{
    m_fadeOutStarted = 0;
    m_fadeOutSamples = 0;
    m_currFadeOut = 0;
    emit stopped();
    return 1;
}

int playbackCallback(void *outputBuffer, void *inputBuffer, unsigned int nFrames, double streamTime, RtAudioStreamStatus status, void *userData)
{
    Q_UNUSED(inputBuffer)
    Q_UNUSED(streamTime)
    Q_UNUSED(status)

    PlaybackManager* playbackManager = static_cast<PlaybackManager*>(userData);
    return playbackManager->writeNextAudioData(outputBuffer, nFrames);
}

void playbackErrorCallback(RtAudioError::Type type, const std::string &errorText)
{
    Q_UNUSED(type)
    qDebug() << "\nError with RtAudio playback" << errorText.c_str() << '\n';
}
