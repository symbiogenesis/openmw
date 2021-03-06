#include "soundmanagerimp.hpp"

#include <algorithm>
#include <map>
#include <numeric>

#include <osg/Matrixf>

#include <components/misc/rng.hpp>
#include <components/debug/debuglog.hpp>
#include <components/vfs/manager.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/statemanager.hpp"

#include "../mwworld/esmstore.hpp"
#include "../mwworld/cellstore.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "sound_buffer.hpp"
#include "sound_decoder.hpp"
#include "sound_output.hpp"
#include "sound.hpp"

#include "openal_output.hpp"
#include "ffmpeg_decoder.hpp"


namespace MWSound
{
    namespace
    {
        constexpr float sMinUpdateInterval = 1.0f / 30.0f;

        constexpr std::chrono::steady_clock::duration sAsyncOperationTimeout = std::chrono::milliseconds(200);

        template <class Function>
        class WorkItem final : public SceneUtil::WorkItem
        {
        public:
            WorkItem(Function&& function) : mFunction(std::move(function)) {}

            void doWork() final
            {
                if (mAborted)
                    return;
                mFunction();
            }

            void abort() final { mAborted = true; }

        private:
            std::atomic_bool mAborted {false};
            Function mFunction;
        };

        template <class Function>
        auto makeWorkItem(Function&& function)
        {
            using WorkItem = WorkItem<std::decay_t<Function>>;
            return osg::ref_ptr<WorkItem>(new WorkItem(std::forward<Function>(function)));
        }

        WaterSoundUpdaterSettings makeWaterSoundUpdaterSettings()
        {
            WaterSoundUpdaterSettings settings;

            settings.mNearWaterRadius = Fallback::Map::getInt("Water_NearWaterRadius");
            settings.mNearWaterPoints = Fallback::Map::getInt("Water_NearWaterPoints");
            settings.mNearWaterIndoorTolerance = Fallback::Map::getFloat("Water_NearWaterIndoorTolerance");
            settings.mNearWaterOutdoorTolerance = Fallback::Map::getFloat("Water_NearWaterOutdoorTolerance");
            settings.mNearWaterIndoorID = Misc::StringUtils::lowerCase(Fallback::Map::getString("Water_NearWaterIndoorID"));
            settings.mNearWaterOutdoorID = Misc::StringUtils::lowerCase(Fallback::Map::getString("Water_NearWaterOutdoorID"));

            return settings;
        }

        template <class Waiting>
        void abortAll(Waiting& waiting)
        {
            for (const auto& v : waiting)
                v.mWorkItem->abort();
        }

        template <class Waiting>
        void waitForAll(Waiting& waiting)
        {
            for (const auto& v : waiting)
                v.mWorkItem->waitTillDone();
        }

        float getMinDistance(const MWBase::World& world)
        {
            const float fAudioMinDistanceMult = world.getStore().get<ESM::GameSetting>().find("fAudioMinDistanceMult")->mValue.getFloat();
            const float fAudioVoiceDefaultMinDistance = world.getStore().get<ESM::GameSetting>().find("fAudioVoiceDefaultMinDistance")->mValue.getFloat();

            return std::max(fAudioVoiceDefaultMinDistance * fAudioMinDistanceMult, 1.0f);
        }

        float getMaxDistance(const MWBase::World& world)
        {
            const float fAudioMaxDistanceMult = world.getStore().get<ESM::GameSetting>().find("fAudioMaxDistanceMult")->mValue.getFloat();
            const float fAudioVoiceDefaultMaxDistance = world.getStore().get<ESM::GameSetting>().find("fAudioVoiceDefaultMaxDistance")->mValue.getFloat();

            return std::max(fAudioVoiceDefaultMaxDistance * fAudioMaxDistanceMult, getMinDistance(world));
        }
    }

    // For combining PlayMode and Type flags
    inline int operator|(PlayMode a, Type b) { return static_cast<int>(a) | static_cast<int>(b); }

    SoundManager::SoundManager(const VFS::Manager* vfs, bool useSound)
        : mVFS(vfs)
        , mOutput(new DEFAULT_OUTPUT(*this))
        , mWaterSoundUpdater(makeWaterSoundUpdaterSettings())
        , mSoundBuffers(new SoundBufferList::element_type())
        , mBufferCacheSize(0)
        , mListenerUnderwater(false)
        , mListenerPos(0,0,0)
        , mListenerDir(1,0,0)
        , mListenerUp(0,0,1)
        , mUnderwaterSound(nullptr)
        , mNearWaterSound(nullptr)
        , mPlaybackPaused(false)
        , mWorkQueue(new SceneUtil::WorkQueue(1))
    {
        mBufferCacheMin = std::max(Settings::Manager::getInt("buffer cache min", "Sound"), 1);
        mBufferCacheMax = std::max(Settings::Manager::getInt("buffer cache max", "Sound"), 1);
        mBufferCacheMax *= 1024*1024;
        mBufferCacheMin = std::min(mBufferCacheMin*1024*1024, mBufferCacheMax);

        if(!useSound)
        {
            Log(Debug::Info) << "Sound disabled.";
            return;
        }

        std::string hrtfname = Settings::Manager::getString("hrtf", "Sound");
        int hrtfstate = Settings::Manager::getInt("hrtf enable", "Sound");
        HrtfMode hrtfmode = hrtfstate < 0 ? HrtfMode::Auto :
                            hrtfstate > 0 ? HrtfMode::Enable : HrtfMode::Disable;

        std::string devname = Settings::Manager::getString("device", "Sound");
        if(!mOutput->init(devname, hrtfname, hrtfmode))
        {
            Log(Debug::Error) << "Failed to initialize audio output, sound disabled";
            return;
        }

        std::vector<std::string> names = mOutput->enumerate();
        std::stringstream stream;

        stream << "Enumerated output devices:\n";
        for(const std::string &name : names)
            stream << "  " << name;

        Log(Debug::Info) << stream.str();
        stream.str("");

        names = mOutput->enumerateHrtf();
        if(!names.empty())
        {
            stream << "Enumerated HRTF names:\n";
            for(const std::string &name : names)
                stream << "  " << name;

            Log(Debug::Info) << stream.str();
        }
    }

    SoundManager::~SoundManager()
    {
        clear();
        for(Sound_Buffer &sfx : *mSoundBuffers)
        {
            if(sfx.mHandle)
                mOutput->unloadSound(sfx.mHandle);
            sfx.mHandle = 0;
        }
        mUnusedBuffers.clear();
        mOutput.reset();
    }

    // Return a new decoder instance, used as needed by the output implementations
    DecoderPtr SoundManager::getDecoder() const
    {
        return DecoderPtr(new DEFAULT_DECODER (mVFS));
    }

    Sound_Buffer *SoundManager::insertSound(const std::string &soundId, const ESM::Sound *sound)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        static const float fAudioDefaultMinDistance = world->getStore().get<ESM::GameSetting>().find("fAudioDefaultMinDistance")->mValue.getFloat();
        static const float fAudioDefaultMaxDistance = world->getStore().get<ESM::GameSetting>().find("fAudioDefaultMaxDistance")->mValue.getFloat();
        static const float fAudioMinDistanceMult = world->getStore().get<ESM::GameSetting>().find("fAudioMinDistanceMult")->mValue.getFloat();
        static const float fAudioMaxDistanceMult = world->getStore().get<ESM::GameSetting>().find("fAudioMaxDistanceMult")->mValue.getFloat();
        float volume, min, max;

        volume = static_cast<float>(pow(10.0, (sound->mData.mVolume / 255.0*3348.0 - 3348.0) / 2000.0));
        min = sound->mData.mMinRange;
        max = sound->mData.mMaxRange;
        if (min == 0 && max == 0)
        {
            min = fAudioDefaultMinDistance;
            max = fAudioDefaultMaxDistance;
        }

        min *= fAudioMinDistanceMult;
        max *= fAudioMaxDistanceMult;
        min = std::max(min, 1.0f);
        max = std::max(min, max);

        Sound_Buffer *sfx = &*mSoundBuffers->insert(mSoundBuffers->end(),
            Sound_Buffer("Sound/"+sound->mSound, volume, min, max)
        );
        mVFS->normalizeFilename(sfx->mResourceName);

        mBufferNameMap.insert(std::make_pair(soundId, sfx));

        return sfx;
    }

    // Lookup a soundId for its sound data (resource name, local volume,
    // minRange, and maxRange)
    Sound_Buffer *SoundManager::lookupSound(const std::string &soundId) const
    {
        NameBufferMap::const_iterator snd = mBufferNameMap.find(soundId);
        if(snd != mBufferNameMap.end())
        {
            Sound_Buffer *sfx = snd->second;
            if(sfx->mHandle) return sfx;
        }
        return nullptr;
    }

    // Lookup a soundId for its sound data (resource name, local volume,
    // minRange, and maxRange), and ensure it's ready for use.
    Sound_Buffer *SoundManager::loadSoundSync(const std::string &soundId)
    {
#ifdef __GNUC__
#define LIKELY(x) __builtin_expect((bool)(x), true)
#define UNLIKELY(x) __builtin_expect((bool)(x), false)
#else
#define LIKELY(x) (bool)(x)
#define UNLIKELY(x) (bool)(x)
#endif
        if(UNLIKELY(mBufferNameMap.empty()))
        {
            MWBase::World *world = MWBase::Environment::get().getWorld();
            for(const ESM::Sound &sound : world->getStore().get<ESM::Sound>())
                insertSound(Misc::StringUtils::lowerCase(sound.mId), &sound);
        }

        Sound_Buffer *sfx;
        NameBufferMap::const_iterator snd = mBufferNameMap.find(soundId);
        if(LIKELY(snd != mBufferNameMap.end()))
            sfx = snd->second;
        else
        {
            MWBase::World *world = MWBase::Environment::get().getWorld();
            const ESM::Sound *sound = world->getStore().get<ESM::Sound>().search(soundId);
            if(!sound) return nullptr;
            sfx = insertSound(soundId, sound);
        }
#undef LIKELY
#undef UNLIKELY

        if(!sfx->mHandle)
        {
            size_t size;
            std::tie(sfx->mHandle, size) = mOutput->loadSound(sfx->mResourceName);
            if(!sfx->mHandle) return nullptr;

            mBufferCacheSize += size;
            if(mBufferCacheSize > mBufferCacheMax)
            {
                do {
                    if(mUnusedBuffers.empty())
                    {
                        Log(Debug::Warning) << "No unused sound buffers to free, using " << mBufferCacheSize << " bytes!";
                        break;
                    }
                    Sound_Buffer *unused = mUnusedBuffers.back();

                    size = mOutput->unloadSound(unused->mHandle);
                    mBufferCacheSize -= size;
                    unused->mHandle = 0;

                    mUnusedBuffers.pop_back();
                } while(mBufferCacheSize > mBufferCacheMin);
            }
            mUnusedBuffers.push_front(sfx);
        }

        return sfx;
    }

    DecoderPtr SoundManager::loadVoice(const std::string &voicefile) const
    {
        try
        {
            DecoderPtr decoder = getDecoder();

            // Workaround: Bethesda at some point converted some of the files to mp3, but the references were kept as .wav.
            if(mVFS->exists(voicefile))
                decoder->open(voicefile);
            else
            {
                std::string file = voicefile;
                std::string::size_type pos = file.rfind('.');
                if(pos != std::string::npos)
                    file = file.substr(0, pos)+".mp3";
                decoder->open(file);
            }

            return decoder;
        }
        catch(std::exception &e)
        {
            Log(Debug::Error) << "Failed to load audio from " << voicefile << ": " << e.what();
        }

        return nullptr;
    }

    SoundPtr SoundManager::getSoundRef()
    {
        return mSounds.get();
    }

    StreamPtr SoundManager::getStreamRef()
    {
        return mStreams.get();
    }

    bool SoundManager::playVoice(DecoderPtr decoder, bool playlocal, Stream* stream)
    {
        if (playlocal)
            return mOutput->streamSound(decoder, stream, true);
        else
            return mOutput->streamSound3D(decoder, stream, true);
    }

    // Gets the combined volume settings for the given sound type
    float SoundManager::volumeFromType(Type type) const
    {
        return mVolumeSettings.getVolumeFromType(type);
    }

    void SoundManager::stopMusic()
    {
        if(mMusic)
        {
            mOutput->finishStream(mMusic.get());
            mMusic = nullptr;
        }
    }

    void SoundManager::streamMusicFull(const std::string& filename)
    {
        if(!mOutput->isInitialized())
            return;
        Log(Debug::Info) << "Playing " << filename;
        mLastPlayedMusic = filename;

        const auto createDecoder = makeWorkItem([this, fileName = filename] { createMusicDecoder(fileName); });
        const auto deadline = std::chrono::steady_clock::now() + sAsyncOperationTimeout;
        mWaitingMusic.emplace_back(Music {filename, createDecoder, deadline});
        mWorkQueue->addWorkItem(createDecoder);
    }

    void SoundManager::advanceMusic(const std::string& filename)
    {
        if (!isMusicPlaying())
        {
            streamMusicFull(filename);
            return;
        }

        mNextMusic = filename;

        mMusic->setFadeout(1.f);
    }

    void SoundManager::startRandomTitle()
    {
        const std::vector<std::string> &filelist = mMusicFiles[mCurrentPlaylist];
        auto &tracklist = mMusicToPlay[mCurrentPlaylist];

        // Do a Fisher-Yates shuffle

        // Repopulate if playlist is empty
        if(tracklist.empty())
        {
            tracklist.resize(filelist.size());
            std::iota(tracklist.begin(), tracklist.end(), 0);
        }

        int i = Misc::Rng::rollDice(tracklist.size());

        // Reshuffle if last played music is the same after a repopulation
        if(filelist[tracklist[i]] == mLastPlayedMusic)
            i = (i+1) % tracklist.size();

        // Remove music from list after advancing music
        advanceMusic(filelist[tracklist[i]]);
        tracklist[i] = tracklist.back();
        tracklist.pop_back();
    }


    void SoundManager::streamMusic(const std::string& filename)
    {
        advanceMusic("Music/"+filename);
    }

    bool SoundManager::isMusicPlaying()
    {
        return !mWaitingMusic.empty() || (mMusic && mOutput->isStreamPlaying(mMusic.get()));
    }

    void SoundManager::playPlaylist(const std::string &playlist)
    {
        if (mCurrentPlaylist == playlist)
            return;

        if (mMusicFiles.find(playlist) == mMusicFiles.end())
        {
            std::vector<std::string> filelist;
            const std::map<std::string, VFS::File*>& index = mVFS->getIndex();

            std::string pattern = "Music/" + playlist;
            mVFS->normalizeFilename(pattern);

            std::map<std::string, VFS::File*>::const_iterator found = index.lower_bound(pattern);
            while (found != index.end())
            {
                if (found->first.size() >= pattern.size() && found->first.substr(0, pattern.size()) == pattern)
                    filelist.push_back(found->first);
                else
                    break;
                ++found;
            }

            mMusicFiles[playlist] = filelist;
        }

        if (mMusicFiles[playlist].empty())
            return;

        mCurrentPlaylist = playlist;
        startRandomTitle();
    }

    void SoundManager::playTitleMusic()
    {
        if (mCurrentPlaylist == "Title")
            return;

        if (mMusicFiles.find("Title") == mMusicFiles.end())
        {
            std::vector<std::string> filelist;
            const std::map<std::string, VFS::File*>& index = mVFS->getIndex();
            // Is there an ini setting for this filename or something?
            std::string filename = "music/special/morrowind title.mp3";
            auto found = index.find(filename);
            if (found != index.end())
            {
                filelist.emplace_back(found->first);
                mMusicFiles["Title"] = filelist;
            }
            else
            {
                Log(Debug::Warning) << "Title music not found";
                return;
            }
        }

        if (mMusicFiles["Title"].empty())
            return;

        mCurrentPlaylist = "Title";
        startRandomTitle();
    }

    void SoundManager::say(const MWWorld::ConstPtr &ptr, const std::string &filename)
    {
        if(!mOutput->isInitialized())
            return;

        sayAsync(ptr, filename, mWaitingVoice);
    }

    float SoundManager::getSaySoundLoudness(const MWWorld::ConstPtr &ptr) const
    {
        SaySoundMap::const_iterator snditer = mActiveSaySounds.find(ptr);
        if(snditer != mActiveSaySounds.end())
        {
            Stream *sound = snditer->second.get();
            return mOutput->getStreamLoudness(sound);
        }

        return 0.0f;
    }

    void SoundManager::say(const std::string& filename)
    {
        if(!mOutput->isInitialized())
            return;

        sayAsync(MWWorld::ConstPtr(), filename, mActiveWaitingVoice);
    }

    bool SoundManager::sayDone(const MWWorld::ConstPtr &ptr) const
    {
        SaySoundMap::const_iterator snditer = mActiveSaySounds.find(ptr);
        if(snditer != mActiveSaySounds.end())
        {
            if(mOutput->isStreamPlaying(snditer->second.get()))
                return false;
            return true;
        }

        const auto isPtr = [&] (const Voice& v) { return v.mPtr == ptr; };

        const auto activeWaiting = std::find_if(mActiveWaitingVoice.begin(), mActiveWaitingVoice.end(), isPtr);
        if (activeWaiting != mActiveWaitingVoice.end())
            return false;

        return true;
    }

    bool SoundManager::sayActive(const MWWorld::ConstPtr &ptr) const
    {
        SaySoundMap::const_iterator snditer = mSaySoundsQueue.find(ptr);
        if(snditer != mSaySoundsQueue.end())
        {
            if(mOutput->isStreamPlaying(snditer->second.get()))
                return true;
            return false;
        }

        snditer = mActiveSaySounds.find(ptr);
        if(snditer != mActiveSaySounds.end())
        {
            if(mOutput->isStreamPlaying(snditer->second.get()))
                return true;
            return false;
        }

        const auto isPtr = [&] (const Voice& v) { return v.mPtr == ptr; };

        const auto waiting = std::find_if(mWaitingVoice.begin(), mWaitingVoice.end(), isPtr);
        if (waiting != mWaitingVoice.end())
            return true;

        const auto activeWaiting = std::find_if(mActiveWaitingVoice.begin(), mActiveWaitingVoice.end(), isPtr);
        if (activeWaiting != mActiveWaitingVoice.end())
            return true;

        return false;
    }

    void SoundManager::stopSay(const MWWorld::ConstPtr &ptr)
    {
        const auto isPtr = [&] (const Voice& v) { return v.mPtr == ptr; };

        mWaitingVoice.erase(std::remove_if(mWaitingVoice.begin(), mWaitingVoice.end(), isPtr), mWaitingVoice.end());
        mActiveWaitingVoice.erase(std::remove_if(mActiveWaitingVoice.begin(), mActiveWaitingVoice.end(), isPtr), mActiveWaitingVoice.end());

        SaySoundMap::iterator snditer = mSaySoundsQueue.find(ptr);
        if(snditer != mSaySoundsQueue.end())
        {
            mOutput->finishStream(snditer->second.get());
            mSaySoundsQueue.erase(snditer);
        }

        snditer = mActiveSaySounds.find(ptr);
        if(snditer != mActiveSaySounds.end())
        {
            mOutput->finishStream(snditer->second.get());
            mActiveSaySounds.erase(snditer);
        }
    }


    Stream *SoundManager::playTrack(const DecoderPtr& decoder, Type type)
    {
        if(!mOutput->isInitialized())
            return nullptr;

        StreamPtr track = getStreamRef();
        track->init([&] {
            SoundParams params;
            params.mBaseVolume = volumeFromType(type);
            params.mFlags = PlayMode::NoEnv | type | Play_2D;
            return params;
        } ());

        if(!mOutput->streamSound(decoder, track.get()))
            return nullptr;

        track->setPlaying();

        Stream* result = track.get();
        const auto it = std::lower_bound(mActiveTracks.begin(), mActiveTracks.end(), track);
        mActiveTracks.insert(it, std::move(track));
        return result;
    }

    void SoundManager::stopTrack(Stream *stream)
    {
        mOutput->finishStream(stream);
        TrackList::iterator iter = std::lower_bound(mActiveTracks.begin(), mActiveTracks.end(), stream,
                                                    [] (const StreamPtr& lhs, Stream* rhs) { return lhs.get() < rhs; });
        if(iter != mActiveTracks.end() && iter->get() == stream)
            mActiveTracks.erase(iter);
    }

    double SoundManager::getTrackTimeDelay(Stream *stream)
    {
        return mOutput->getStreamDelay(stream);
    }


    Sound* SoundManager::playSound(const std::string& soundId, float volume, float pitch, Type type, PlayMode mode, float offset)
    {
        if(!mOutput->isInitialized())
            return nullptr;

        SoundPtr sound = getSoundRef();

        sound->init([&] {
            SoundParams params;
            params.mVolumeFactor = volume;
            params.mSfxVolume = 0.0f;
            params.mBaseVolume = volumeFromType(type);
            params.mPitch = pitch;
            params.mFlags = mode | type | Play_2D;
            return params;
        } ());

        Sound* result = sound.get();

        loadSoundAsync(MWWorld::ConstPtr(), Misc::StringUtils::lowerCase(soundId), offset, std::move(sound));

        return result;
    }

    Sound *SoundManager::playSound3D(const MWWorld::ConstPtr &ptr, const std::string& soundId,
                                     float volume, float pitch, Type type, PlayMode mode,
                                     float offset)
    {
        if(!mOutput->isInitialized())
            return nullptr;

        const osg::Vec3f objpos(ptr.getRefData().getPosition().asVec3());
        if((mode & PlayMode::RemoveAtDistance) && (mListenerPos - objpos).length2() > 2000*2000)
            return nullptr;

        SoundPtr sound = getSoundRef();

        if (!(mode & PlayMode::NoPlayerLocal) && ptr == MWMechanics::getPlayer())
        {
            sound->init([&] {
                SoundParams params;
                params.mVolumeFactor = volume;
                params.mSfxVolume = 0.0f;
                params.mBaseVolume = volumeFromType(type);
                params.mPitch = pitch;
                params.mFlags = mode | type | Play_2D;
                return params;
            } ());
        }
        else
        {
            sound->init([&] {
                SoundParams params;
                params.mPos = objpos;
                params.mVolumeFactor = volume;
                params.mSfxVolume = 0.0f;
                params.mBaseVolume = volumeFromType(type);
                params.mPitch = pitch;
                params.mMinDistance = 0.0f;
                params.mMaxDistance = 0.0f;
                params.mFlags = mode | type | Play_3D;
                return params;
            } ());
        }

        Sound* result = sound.get();

        // Look up the sound in the ESM data
        loadSoundAsync(ptr, Misc::StringUtils::lowerCase(soundId), offset, std::move(sound));

        return result;
    }

    Sound *SoundManager::playSound3D(const osg::Vec3f& initialPos, const std::string& soundId,
                                     float volume, float pitch, Type type, PlayMode mode,
                                     float offset)
    {
        if(!mOutput->isInitialized())
            return nullptr;

        SoundPtr sound = getSoundRef();

        sound->init([&] {
            SoundParams params;
            params.mPos = initialPos;
            params.mVolumeFactor = volume;
            params.mSfxVolume = 0.0f;
            params.mBaseVolume = volumeFromType(type);
            params.mPitch = pitch;
            params.mMinDistance = 0.0f;
            params.mMaxDistance = 0.0f;
            params.mFlags = mode | type | Play_3D;
            return params;
        } ());

        Sound* result = sound.get();

        // Look up the sound in the ESM data
        loadSoundAsync(MWWorld::ConstPtr(), Misc::StringUtils::lowerCase(soundId), offset, std::move(sound));

        return result;
    }

    void SoundManager::stopSound(Sound *sound)
    {
        if (!sound)
            return;

        if (sound->isPlaying())
            mOutput->finishSound(sound);
        else
            sound->cancelLoading();
    }

    void SoundManager::stopSound(Sound_Buffer *sfx, const MWWorld::ConstPtr &ptr)
    {
        SoundMap::iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            for(SoundBufferRefPair &snd : snditer->second)
            {
                if(snd.second == sfx)
                    mOutput->finishSound(snd.first.get());
            }
        }
    }

    void SoundManager::stopSound3D(const MWWorld::ConstPtr &ptr, const std::string& soundId)
    {
        if(!mOutput->isInitialized())
            return;

        std::string normalizedSoundId = Misc::StringUtils::lowerCase(soundId);

        if (Sound_Buffer *sfx = lookupSound(normalizedSoundId))
            stopSound(sfx, ptr);

        for (const auto& loading : mLoadingSounds)
            if (loading.mPtr == ptr && loading.mSoundId == normalizedSoundId)
                loading.mSound->cancelLoading();
    }

    void SoundManager::stopSound3D(const MWWorld::ConstPtr &ptr)
    {
        for (const auto& loading : mLoadingSounds)
            if (loading.mPtr == ptr)
                loading.mSound->cancelLoading();

        SoundMap::iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            for(SoundBufferRefPair &snd : snditer->second)
                mOutput->finishSound(snd.first.get());
        }
        SaySoundMap::iterator sayiter = mSaySoundsQueue.find(ptr);
        if(sayiter != mSaySoundsQueue.end())
            mOutput->finishStream(sayiter->second.get());
        sayiter = mActiveSaySounds.find(ptr);
        if(sayiter != mActiveSaySounds.end())
            mOutput->finishStream(sayiter->second.get());
    }

    void SoundManager::stopSound(const MWWorld::CellStore *cell)
    {
        for (const auto& loading : mLoadingSounds)
            if (!loading.mPtr.isEmpty() && loading.mPtr != MWMechanics::getPlayer() && loading.mPtr.getCell() == cell)
                loading.mSound->cancelLoading();

        for(SoundMap::value_type &snd : mActiveSounds)
        {
            if(!snd.first.isEmpty() && snd.first != MWMechanics::getPlayer() && snd.first.getCell() == cell)
            {
                for(SoundBufferRefPair &sndbuf : snd.second)
                    mOutput->finishSound(sndbuf.first.get());
            }
        }

        for(SaySoundMap::value_type &snd : mSaySoundsQueue)
        {
            if(!snd.first.isEmpty() && snd.first != MWMechanics::getPlayer() && snd.first.getCell() == cell)
                mOutput->finishStream(snd.second.get());
        }

        for(SaySoundMap::value_type &snd : mActiveSaySounds)
        {
            if(!snd.first.isEmpty() && snd.first != MWMechanics::getPlayer() && snd.first.getCell() == cell)
                mOutput->finishStream(snd.second.get());
        }
    }

    void SoundManager::fadeOutSound3D(const MWWorld::ConstPtr &ptr,
            const std::string& soundId, float duration)
    {
        SoundMap::iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            Sound_Buffer *sfx = lookupSound(Misc::StringUtils::lowerCase(soundId));
            if (!sfx)
                return;
            for(SoundBufferRefPair &sndbuf : snditer->second)
            {
                if(sndbuf.second == sfx)
                    sndbuf.first->setFadeout(duration);
            }
        }
    }

    bool SoundManager::getSoundPlaying(const MWWorld::ConstPtr &ptr, const std::string& soundId) const
    {
        SoundMap::const_iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            Sound_Buffer *sfx = lookupSound(Misc::StringUtils::lowerCase(soundId));
            const bool playing = std::find_if(snditer->second.cbegin(), snditer->second.cend(),
                [this,sfx](const SoundBufferRefPair &snd) -> bool
                { return snd.second == sfx && mOutput->isSoundPlaying(snd.first.get()); }
            ) != snditer->second.cend();
            if (playing)
                return true;
        }
        return mLoadingSounds.end() != std::find_if(mLoadingSounds.begin(), mLoadingSounds.end(),
            [&] (const LoadingSound& v) { return v.mPtr == ptr && v.mSoundId == soundId; });
    }

    void SoundManager::pauseSounds(BlockerType blocker, int types)
    {
        if(mOutput->isInitialized())
        {
            if (mPausedSoundTypes[blocker] != 0)
                resumeSounds(blocker);

            types = types & Type::Mask;
            mOutput->pauseSounds(types);
            mPausedSoundTypes[blocker] = types;
        }
    }

    void SoundManager::resumeSounds(BlockerType blocker)
    {
        if(mOutput->isInitialized())
        {
            mPausedSoundTypes[blocker] = 0;
            int types = int(Type::Mask);
            for (int currentBlocker = 0; currentBlocker < BlockerType::MaxCount; currentBlocker++)
            {
                if (currentBlocker != blocker)
                    types &= ~mPausedSoundTypes[currentBlocker];
            }

            mOutput->resumeSounds(types);
        }
    }

    void SoundManager::pausePlayback()
    {
        if (mPlaybackPaused)
            return;

        mPlaybackPaused = true;
        mOutput->pauseActiveDevice();
    }

    void SoundManager::resumePlayback()
    {
        if (!mPlaybackPaused)
            return;

        mPlaybackPaused = false;
        mOutput->resumeActiveDevice();
    }

    void SoundManager::updateRegionSound(float duration)
    {
        MWBase::World *world = MWBase::Environment::get().getWorld();
        const MWWorld::ConstPtr player = world->getPlayerPtr();
        const ESM::Cell *cell = player.getCell()->getCell();

        if (!cell->isExterior())
            return;

        if (const auto next = mRegionSoundSelector.getNextRandom(duration, cell->mRegion, *world))
            playSound(*next, 1.0f, 1.0f);
    }

    void SoundManager::updateWaterSound()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::ConstPtr player = world->getPlayerPtr();
        const ESM::Cell *curcell = player.getCell()->getCell();
        const auto update = mWaterSoundUpdater.update(player, *world);

        WaterSoundAction action;
        Sound_Buffer* sfx;
        std::tie(action, sfx) = getWaterSoundAction(update, curcell);

        switch (action)
        {
            case WaterSoundAction::DoNothing:
                break;
            case WaterSoundAction::SetVolume:
                mNearWaterSound->setVolumeFactor(update.mVolume);
                mNearWaterSound->setSfxVolume(sfx->mVolume);
                break;
            case WaterSoundAction::FinishSound:
                mOutput->finishSound(mNearWaterSound);
                mNearWaterSound = nullptr;
                break;
            case WaterSoundAction::PlaySound:
                if (mNearWaterSound)
                    mOutput->finishSound(mNearWaterSound);
                mNearWaterSound = playSound(update.mId, update.mVolume, 1.0f, Type::Sfx, PlayMode::Loop);
                break;
        }

        mLastCell = curcell;
    }

    std::pair<SoundManager::WaterSoundAction, Sound_Buffer*> SoundManager::getWaterSoundAction(
            const WaterSoundUpdate& update, const ESM::Cell* cell) const
    {
        if (mNearWaterSound)
        {
            if (update.mVolume == 0.0f)
                return {WaterSoundAction::FinishSound, nullptr};

            bool soundIdChanged = false;

            Sound_Buffer* sfx = lookupSound(update.mId);
            if (mLastCell != cell)
            {
                const auto snditer = mActiveSounds.find(MWWorld::ConstPtr());
                if (snditer != mActiveSounds.end())
                {
                    const auto pairiter = std::find_if(
                        snditer->second.begin(), snditer->second.end(),
                        [this](const SoundBufferRefPairList::value_type &item) -> bool
                        { return mNearWaterSound == item.first.get(); }
                    );
                    if (pairiter != snditer->second.end() && pairiter->second != sfx)
                        soundIdChanged = true;
                }
            }

            if (soundIdChanged)
                return {WaterSoundAction::PlaySound, nullptr};

            if (sfx)
                return {WaterSoundAction::SetVolume, sfx};
        }
        else if (update.mVolume > 0.0f)
            return {WaterSoundAction::PlaySound, nullptr};

        return {WaterSoundAction::DoNothing, nullptr};
    }

    void SoundManager::updateSounds(float duration)
    {
        // We update active say sounds map for specific actors here
        // because for vanilla compatibility we can't do it immediately.
        SaySoundMap::iterator queuesayiter = mSaySoundsQueue.begin();
        while (queuesayiter != mSaySoundsQueue.end())
        {
            const auto dst = mActiveSaySounds.find(queuesayiter->first);
            if (dst == mActiveSaySounds.end())
                mActiveSaySounds.emplace(queuesayiter->first, std::move(queuesayiter->second));
            else
                dst->second = std::move(queuesayiter->second);
            mSaySoundsQueue.erase(queuesayiter++);
        }

        mTimePassed += duration;
        if (mTimePassed < sMinUpdateInterval)
            return;
        duration = mTimePassed;
        mTimePassed = 0.0f;

        // Make sure music is still playing
        if(!isMusicPlaying() && !mCurrentPlaylist.empty())
            startRandomTitle();

        Environment env = Env_Normal;
        if (mListenerUnderwater)
            env = Env_Underwater;
        else if(mUnderwaterSound)
        {
            mOutput->finishSound(mUnderwaterSound);
            mUnderwaterSound = nullptr;
        }

        mOutput->startUpdate();
        mOutput->updateListener(
            mListenerPos,
            mListenerDir,
            mListenerUp,
            env
        );

        updateMusic(duration);

        // Check if any sounds are finished playing, and trash them
        SoundMap::iterator snditer = mActiveSounds.begin();
        while(snditer != mActiveSounds.end())
        {
            MWWorld::ConstPtr ptr = snditer->first;
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            while(sndidx != snditer->second.end())
            {
                Sound *sound = sndidx->first.get();
                Sound_Buffer *sfx = sndidx->second;

                if(!ptr.isEmpty() && sound->getIs3D())
                {
                    const ESM::Position &pos = ptr.getRefData().getPosition();
                    const osg::Vec3f objpos(pos.asVec3());
                    sound->setPosition(objpos);

                    if(sound->getDistanceCull())
                    {
                        if((mListenerPos - objpos).length2() > 2000*2000)
                            mOutput->finishSound(sound);
                    }
                }

                if(!mOutput->isSoundPlaying(sound))
                {
                    mOutput->finishSound(sound);
                    if (sound == mUnderwaterSound)
                        mUnderwaterSound = nullptr;
                    if (sound == mNearWaterSound)
                        mNearWaterSound = nullptr;
                    if(sfx->mUses-- == 1)
                        mUnusedBuffers.push_front(sfx);
                    sndidx = snditer->second.erase(sndidx);
                }
                else
                {
                    sound->updateFade(duration);

                    mOutput->updateSound(sound);
                    ++sndidx;
                }
            }
            if(snditer->second.empty())
                snditer = mActiveSounds.erase(snditer);
            else
                ++snditer;
        }

        SaySoundMap::iterator sayiter = mActiveSaySounds.begin();
        while(sayiter != mActiveSaySounds.end())
        {
            MWWorld::ConstPtr ptr = sayiter->first;
            Stream *sound = sayiter->second.get();
            if(!ptr.isEmpty() && sound->getIs3D())
            {
                MWBase::World *world = MWBase::Environment::get().getWorld();
                const osg::Vec3f pos = world->getActorHeadTransform(ptr).getTrans();
                sound->setPosition(pos);

                if(sound->getDistanceCull())
                {
                    if((mListenerPos - pos).length2() > 2000*2000)
                        mOutput->finishStream(sound);
                }
            }

            if(!mOutput->isStreamPlaying(sound))
            {
                mOutput->finishStream(sound);
                mActiveSaySounds.erase(sayiter++);
            }
            else
            {
                sound->updateFade(duration);

                mOutput->updateStream(sound);
                ++sayiter;
            }
        }

        TrackList::iterator trkiter = mActiveTracks.begin();
        for(;trkiter != mActiveTracks.end();++trkiter)
        {
            Stream *sound = trkiter->get();
            if(!mOutput->isStreamPlaying(sound))
            {
                mOutput->finishStream(sound);
                trkiter = mActiveTracks.erase(trkiter);
            }
            else
            {
                sound->updateFade(duration);

                mOutput->updateStream(sound);
                ++trkiter;
            }
        }

        if(mListenerUnderwater)
        {
            // Play underwater sound (after updating sounds)
            if(!mUnderwaterSound)
                mUnderwaterSound = playSound("Underwater", 1.0f, 1.0f, Type::Sfx, PlayMode::LoopNoEnv);
        }
        mOutput->finishUpdate();
    }


    void SoundManager::updateMusic(float duration)
    {
        if (!mNextMusic.empty())
        {
            mMusic->updateFade(duration);

            mOutput->updateStream(mMusic.get());

            if (mMusic->getRealVolume() <= 0.f)
            {
                streamMusicFull(mNextMusic);
                mNextMusic.clear();
            }
        }
    }


    void SoundManager::update(float duration)
    {
        if(!mOutput->isInitialized() || mPlaybackPaused)
            return;

        playAllVoicesFromCreatedDecoders();
        playMusicFromCreatedDecoder();
        playLoadedSounds();

        updateSounds(duration);
        if (MWBase::Environment::get().getStateManager()->getState()!=
            MWBase::StateManager::State_NoGame)
        {
            updateRegionSound(duration);
            updateWaterSound();
        }
    }


    void SoundManager::processChangedSettings(const Settings::CategorySettingVector& settings)
    {
        mVolumeSettings.update();

        if(!mOutput->isInitialized())
            return;
        mOutput->startUpdate();
        for(SoundMap::value_type &snd : mActiveSounds)
        {
            for(SoundBufferRefPair &sndbuf : snd.second)
            {
                Sound *sound = sndbuf.first.get();
                sound->setBaseVolume(volumeFromType(sound->getPlayType()));
                mOutput->updateSound(sound);
            }
        }
        for(SaySoundMap::value_type &snd : mActiveSaySounds)
        {
            Stream *sound = snd.second.get();
            sound->setBaseVolume(volumeFromType(sound->getPlayType()));
            mOutput->updateStream(sound);
        }
        for(SaySoundMap::value_type &snd : mSaySoundsQueue)
        {
            Stream *sound = snd.second.get();
            sound->setBaseVolume(volumeFromType(sound->getPlayType()));
            mOutput->updateStream(sound);
        }
        for (const StreamPtr& sound : mActiveTracks)
        {
            sound->setBaseVolume(volumeFromType(sound->getPlayType()));
            mOutput->updateStream(sound.get());
        }
        if(mMusic)
        {
            mMusic->setBaseVolume(volumeFromType(mMusic->getPlayType()));
            mOutput->updateStream(mMusic.get());
        }
        mOutput->finishUpdate();
    }

    void SoundManager::setListenerPosDir(const osg::Vec3f &pos, const osg::Vec3f &dir, const osg::Vec3f &up, bool underwater)
    {
        mListenerPos = pos;
        mListenerDir = dir;
        mListenerUp  = up;

        mListenerUnderwater = underwater;

        mWaterSoundUpdater.setUnderwater(underwater);
    }

    void SoundManager::updatePtr(const MWWorld::ConstPtr &old, const MWWorld::ConstPtr &updated)
    {
        SoundMap::iterator snditer = mActiveSounds.find(old);
        if(snditer != mActiveSounds.end())
        {
            SoundBufferRefPairList sndlist = std::move(snditer->second);
            mActiveSounds.erase(snditer);
            mActiveSounds.emplace(updated, std::move(sndlist));
        }

        SaySoundMap::iterator sayiter = mSaySoundsQueue.find(old);
        if(sayiter != mSaySoundsQueue.end())
        {
            StreamPtr stream = std::move(sayiter->second);
            mSaySoundsQueue.erase(sayiter);
            mSaySoundsQueue.emplace(updated, std::move(stream));
        }

        sayiter = mActiveSaySounds.find(old);
        if(sayiter != mActiveSaySounds.end())
        {
            StreamPtr stream = std::move(sayiter->second);
            mActiveSaySounds.erase(sayiter);
            mActiveSaySounds.emplace(updated, std::move(stream));
        }

        for (auto& v : mWaitingVoice)
            if (v.mPtr == old)
                v.mPtr = updated;

        for (auto& v : mLoadingSounds)
            if (v.mPtr == old)
                v.mPtr = updated;
    }

    // Default readAll implementation, for decoders that can't do anything
    // better
    void Sound_Decoder::readAll(std::vector<char> &output)
    {
        size_t total = output.size();
        size_t got;

        output.resize(total+32768);
        while((got=read(&output[total], output.size()-total)) > 0)
        {
            total += got;
            output.resize(total*2);
        }
        output.resize(total);
    }


    const char *getSampleTypeName(SampleType type)
    {
        switch(type)
        {
            case SampleType_UInt8: return "U8";
            case SampleType_Int16: return "S16";
            case SampleType_Float32: return "Float32";
        }
        return "(unknown sample type)";
    }

    const char *getChannelConfigName(ChannelConfig config)
    {
        switch(config)
        {
            case ChannelConfig_Mono:    return "Mono";
            case ChannelConfig_Stereo:  return "Stereo";
            case ChannelConfig_Quad:    return "Quad";
            case ChannelConfig_5point1: return "5.1 Surround";
            case ChannelConfig_7point1: return "7.1 Surround";
        }
        return "(unknown channel config)";
    }

    size_t framesToBytes(size_t frames, ChannelConfig config, SampleType type)
    {
        switch(config)
        {
            case ChannelConfig_Mono:    frames *= 1; break;
            case ChannelConfig_Stereo:  frames *= 2; break;
            case ChannelConfig_Quad:    frames *= 4; break;
            case ChannelConfig_5point1: frames *= 6; break;
            case ChannelConfig_7point1: frames *= 8; break;
        }
        switch(type)
        {
            case SampleType_UInt8: frames *= 1; break;
            case SampleType_Int16: frames *= 2; break;
            case SampleType_Float32: frames *= 4; break;
        }
        return frames;
    }

    size_t bytesToFrames(size_t bytes, ChannelConfig config, SampleType type)
    {
        return bytes / framesToBytes(1, config, type);
    }

    void SoundManager::clear()
    {
        abortAll(mWaitingVoice);
        abortAll(mWaitingMusic);
        abortAll(mLoadingSounds);

        waitForAll(mWaitingVoice);
        waitForAll(mWaitingMusic);
        waitForAll(mLoadingSounds);

        mWaitingVoice.clear();
        mWaitingMusic.clear();
        mLoadingSounds.clear();

        SoundManager::stopMusic();

        for(SoundMap::value_type &snd : mActiveSounds)
        {
            for(SoundBufferRefPair &sndbuf : snd.second)
            {
                mOutput->finishSound(sndbuf.first.get());
                Sound_Buffer *sfx = sndbuf.second;
                if(sfx->mUses-- == 1)
                    mUnusedBuffers.push_front(sfx);
            }
        }
        mActiveSounds.clear();
        mUnderwaterSound = nullptr;
        mNearWaterSound = nullptr;

        for(SaySoundMap::value_type &snd : mSaySoundsQueue)
            mOutput->finishStream(snd.second.get());
        mSaySoundsQueue.clear();

        for(SaySoundMap::value_type &snd : mActiveSaySounds)
            mOutput->finishStream(snd.second.get());
        mActiveSaySounds.clear();

        for(StreamPtr& sound : mActiveTracks)
            mOutput->finishStream(sound.get());
        mActiveTracks.clear();
        mPlaybackPaused = false;
        std::fill(std::begin(mPausedSoundTypes), std::end(mPausedSoundTypes), 0);
    }

    void SoundManager::sayAsync(const MWWorld::ConstPtr &ptr, const std::string &filename, std::vector<Voice>& waiting)
    {
        std::string voicefile = "Sound/" + filename;

        mVFS->normalizeFilename(voicefile);

        StreamPtr stream = getStreamRef();
        const float baseVolume = volumeFromType(Type::Voice);

        if (ptr == MWWorld::ConstPtr())
        {
            stream->init([&] {
                SoundParams params;
                params.mBaseVolume = baseVolume;
                params.mFlags = PlayMode::NoEnv | Type::Voice | Play_2D;
                return params;
            } ());
        }
        else
        {
            const MWBase::World* const world = MWBase::Environment::get().getWorld();
            static const float minDistance = getMinDistance(*world);
            static const float maxDistance = getMaxDistance(*world);
            const osg::Vec3f pos = world->getActorHeadTransform(ptr).getTrans();

            stream->init([&] {
                SoundParams params;
                params.mPos = pos;
                params.mBaseVolume = baseVolume;
                params.mMinDistance = minDistance;
                params.mMaxDistance = maxDistance;
                params.mFlags = PlayMode::Normal | Type::Voice | Play_3D;
                return params;
            } ());
        }

        const auto createDecoder = makeWorkItem([this, fileName = voicefile] { createVoiceDecoder(fileName); });
        const auto deadline = std::chrono::steady_clock::now() + sAsyncOperationTimeout;
        waiting.emplace_back(Voice {ptr, std::move(voicefile), std::move(stream), createDecoder, deadline});
        mWorkQueue->addWorkItem(createDecoder);
    }

    void SoundManager::createVoiceDecoder(const std::string& voicefile)
    {
        {
            const auto locked = mVoiceDecoders.lock();
            if (locked->count(voicefile) > 0)
                return;
        }

        DecoderPtr decoder = loadVoice(voicefile);
        if (!decoder)
            return;

        mVoiceDecoders.lock()->emplace(voicefile, decoder);
    }

    void SoundManager::playAllVoicesFromCreatedDecoders()
    {
        std::move(mWaitingVoice.begin(), mWaitingVoice.end(), std::back_inserter(mActiveWaitingVoice));
        mWaitingVoice.clear();

        if (mActiveWaitingVoice.empty())
            return;

        const auto now = std::chrono::steady_clock::now();
        for (const auto& voice : mActiveWaitingVoice)
            if (voice.mDeadline <= now)
                voice.mWorkItem->waitTillDone();

        const auto locked = mVoiceDecoders.lock();

        for (const auto& decoder : *locked)
        {
            const auto waiting = std::find_if(mActiveWaitingVoice.begin(), mActiveWaitingVoice.end(),
                                              [&] (const Voice& v) { return v.mFileName == decoder.first; });

            if (waiting == mActiveWaitingVoice.end())
                continue;

            const MWWorld::ConstPtr ptr = waiting->mPtr;
            StreamPtr stream = std::move(waiting->mStream);

            stopSay(ptr);

            if (playVoice(decoder.second, ptr == MWMechanics::getPlayer(), stream.get()))
            {
                stream->setPlaying();
                mActiveSaySounds.emplace(ptr, std::move(stream));
            }
        }

        locked->clear();
    }

    void SoundManager::createMusicDecoder(const std::string& fileName)
    {
        {
            const auto locked = mMusicDecoders.lock();
            if (locked->count(fileName) > 0)
                return;
        }

        DecoderPtr decoder = getDecoder();
        decoder->open(fileName);

        mMusicDecoders.lock()->emplace(fileName, decoder);
    }

    void SoundManager::playMusicFromCreatedDecoder()
    {
        if (mWaitingMusic.empty())
            return;

        if (mWaitingMusic.back().mDeadline <= std::chrono::steady_clock::now())
            mWaitingMusic.back().mWorkItem->waitTillDone();

        const auto locked = mMusicDecoders.lock();
        const auto decoder = locked->find(mWaitingMusic.back().mFileName);

        if (decoder == locked->end())
            return;

        stopMusic();

        mMusic = getStreamRef();
        mMusic->init([&] {
            SoundParams params;
            params.mBaseVolume = volumeFromType(Type::Music);
            params.mFlags = PlayMode::NoEnv | Type::Music | Play_2D;
            return params;
        } ());

        mOutput->streamSound(decoder->second, mMusic.get());

        mMusic->setPlaying();
        abortAll(mWaitingMusic);
        mWaitingMusic.clear();
        locked->clear();
    }

    void SoundManager::loadSoundAsync(const MWWorld::ConstPtr& ptr, std::string soundId, float offset, SoundPtr&& sound)
    {
        const auto load = makeWorkItem([this, soundId] { loadSound(soundId); });

        LoadingSound loading;
        loading.mPtr = ptr;
        loading.mSoundId = std::move(soundId);
        loading.mOffset = offset;
        loading.mSound = std::move(sound);
        loading.mWorkItem = load;
        loading.mDeadline = std::chrono::steady_clock::now() + sAsyncOperationTimeout;

        mLoadingSounds.push_back(std::move(loading));
        mWorkQueue->addWorkItem(load);
    }

    void SoundManager::loadSound(const std::string &soundId)
    {
        {
            const auto locked = mLoadedSoundBuffers.lock();
            if (locked->count(soundId) > 0)
                return;
        }

        Sound_Buffer* sfx = loadSoundSync(soundId);

        mLoadedSoundBuffers.lock()->emplace(soundId, sfx);
    }

    void SoundManager::playLoadedSounds()
    {
        if (mLoadingSounds.empty())
            return;

        const auto now = std::chrono::steady_clock::now();
        for (const auto& sound : mLoadingSounds)
            if (sound.mDeadline <= now)
                sound.mWorkItem->waitTillDone();

        const auto locked = mLoadedSoundBuffers.lock();

        for (auto& loading : mLoadingSounds)
        {
            const auto loaded = locked->find(loading.mSoundId);

            if (loaded == locked->end())
                continue;

            SoundPtr sound = std::move(loading.mSound);

            if (sound->isLoadCancelled())
                continue;

            Sound_Buffer* sfx = loaded->second;

            if (sfx == nullptr)
                continue;

            // Only one copy of given sound can be played at time, so stop previous copy
            stopSound(sfx, loading.mPtr);

            sound->setSfxVolume(sfx->mVolume);

            bool played;

            if (sound->getIs3D())
            {
                sound->setMinDistance(sfx->mMinDist);
                sound->setMaxDistance(sfx->mMaxDist);

                played = mOutput->playSound3D(sound.get(), sfx->mHandle, loading.mOffset);
            }
            else
            {
                played = mOutput->playSound(sound.get(), sfx->mHandle, loading.mOffset);
            }

            if (!played)
                continue;

            sound->setPlaying();

            if (sfx->mUses++ == 0)
            {
                const auto iter = std::find(mUnusedBuffers.begin(), mUnusedBuffers.end(), sfx);
                if (iter != mUnusedBuffers.end())
                    mUnusedBuffers.erase(iter);
            }

            mActiveSounds[loading.mPtr].emplace_back(std::move(sound), sfx);
        }

        locked->clear();

        const auto isSoundNull = [] (const LoadingSound& v) { return v.mSound == nullptr; };

        mLoadingSounds.erase(std::remove_if(mLoadingSounds.begin(), mLoadingSounds.end(), isSoundNull), mLoadingSounds.end());
    }
}
