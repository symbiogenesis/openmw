#ifndef GAME_SOUND_SOUND_H
#define GAME_SOUND_SOUND_H

#include <algorithm>

#include "sound_output.hpp"

namespace MWSound
{
    // For testing individual PlayMode flags
    inline int operator&(int a, PlayMode b) { return a & static_cast<int>(b); }
    inline int operator&(PlayMode a, PlayMode b) { return static_cast<int>(a) & static_cast<int>(b); }

    struct SoundParams
    {
        osg::Vec3f mPos{0.0f, 0.0f, 0.0f};
        float mVolumeFactor = 1;
        float mSfxVolume = 1;
        float mBaseVolume = 1;
        float mPitch = 1;
        float mMinDistance = 1;
        float mMaxDistance = 1000;
        int mFlags = 0;
        float mFadeOutTime = 0;
    };

    class SoundBase {
        SoundBase& operator=(const SoundBase&) = delete;
        SoundBase(const SoundBase&) = delete;
        SoundBase(SoundBase&&) = delete;

        SoundParams mParams;

        enum class State
        {
            Loading,
            Playing,
            LoadCancelled,
        };

        State mState = State::Loading;

    protected:
        Sound_Instance mHandle = nullptr;

        friend class OpenAL_Output;

    public:
        void setPosition(const osg::Vec3f &pos) { mParams.mPos = pos; }
        void setVolumeFactor(float value) { mParams.mVolumeFactor = value; }
        void setSfxVolume(float value) { mParams.mSfxVolume = value; }
        void setBaseVolume(float volume) { mParams.mBaseVolume = volume; }
        void setMinDistance(float value) { mParams.mMinDistance = value; }
        void setMaxDistance(float value) { mParams.mMaxDistance = value; }
        void setFadeout(float duration) { mParams.mFadeOutTime = duration; }
        void updateFade(float duration)
        {
            if(mParams.mFadeOutTime > 0.0f)
            {
                float soundDuration = std::min(duration, mParams.mFadeOutTime);
                mParams.mVolumeFactor *= (mParams.mFadeOutTime - soundDuration) / mParams.mFadeOutTime;
                mParams.mFadeOutTime -= soundDuration;
            }
        }
        void setPlaying() { mState = State::Playing; }
        void cancelLoading() { mState = State::LoadCancelled; }

        const osg::Vec3f &getPosition() const { return mParams.mPos; }
        float getRealVolume() const { return mParams.mVolumeFactor * mParams.mSfxVolume * mParams.mBaseVolume; }
        float getPitch() const { return mParams.mPitch; }
        float getMinDistance() const { return mParams.mMinDistance; }
        float getMaxDistance() const { return mParams.mMaxDistance; }

        MWSound::Type getPlayType() const
        { return static_cast<MWSound::Type>(mParams.mFlags & MWSound::Type::Mask); }
        bool getUseEnv() const { return !(mParams.mFlags & MWSound::PlayMode::NoEnv); }
        bool getIsLooping() const { return mParams.mFlags & MWSound::PlayMode::Loop; }
        bool getDistanceCull() const { return mParams.mFlags & MWSound::PlayMode::RemoveAtDistance; }
        bool getIs3D() const { return mParams.mFlags & Play_3D; }
        bool isPlaying() const { return mState == State::Playing; }
        bool isLoadCancelled() const { return mState == State::LoadCancelled; }

        void init(const SoundParams& params)
        {
            mParams = params;
            mState = State::Loading;
            mHandle = nullptr;
        }

        SoundBase() = default;
    };

    class Sound : public SoundBase {
        Sound& operator=(const Sound&) = delete;
        Sound(const Sound&) = delete;
        Sound(Sound&&) = delete;

    public:
        Sound() { }
    };

    class Stream : public SoundBase {
        Stream& operator=(const Stream&) = delete;
        Stream(const Stream&) = delete;
        Stream(Stream&&) = delete;

    public:
        Stream() { }
    };
}

#endif
