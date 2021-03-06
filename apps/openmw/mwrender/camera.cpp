#include "camera.hpp"

#include <osg/Camera>

#include <components/sceneutil/positionattitudetransform.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"

#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "npcanimation.hpp"

namespace
{

class UpdateRenderCameraCallback : public osg::NodeCallback
{
public:
    UpdateRenderCameraCallback(MWRender::Camera* cam)
        : mCamera(cam)
    {
    }

    virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        osg::Camera* cam = static_cast<osg::Camera*>(node);

        // traverse first to update animations, in case the camera is attached to an animated node
        traverse(node, nv);

        mCamera->updateCamera(cam);
    }

private:
    MWRender::Camera* mCamera;
};

}

namespace MWRender
{

    Camera::Camera (osg::Camera* camera)
    : mHeightScale(1.f),
      mCamera(camera),
      mAnimation(nullptr),
      mFirstPersonView(true),
      mPreviewMode(false),
      mNearest(30.f),
      mFurthest(800.f),
      mIsNearest(false),
      mHeight(124.f),
      mBaseCameraDistance(192.f),
      mVanityToggleQueued(false),
      mVanityToggleQueuedValue(false),
      mViewModeToggleQueued(false),
      mCameraDistance(0.f),
      mThirdPersonMode(ThirdPersonViewMode::Standard),
      mOverShoulderOffset(osg::Vec2f(30.0f, -10.0f)),
      mSmoothTransitionToCombatMode(0.f)
    {
        mVanity.enabled = false;
        mVanity.allowed = true;

        mPreviewCam.pitch = 0.f;
        mPreviewCam.yaw = 0.f;
        mPreviewCam.offset = 400.f;
        mMainCam.pitch = 0.f;
        mMainCam.yaw = 0.f;
        mMainCam.offset = 400.f;

        mCameraDistance = mBaseCameraDistance;

        mUpdateCallback = new UpdateRenderCameraCallback(this);
        mCamera->addUpdateCallback(mUpdateCallback);
    }

    Camera::~Camera()
    {
        mCamera->removeUpdateCallback(mUpdateCallback);
    }

    MWWorld::Ptr Camera::getTrackingPtr() const
    {
        return mTrackingPtr;
    }

    osg::Vec3d Camera::getFocalPoint() const
    {
        const osg::Node* trackNode = mTrackingNode;
        if (!trackNode)
            return osg::Vec3d();
        osg::NodePathList nodepaths = trackNode->getParentalNodePaths();
        if (nodepaths.empty())
            return osg::Vec3d();
        osg::Matrix worldMat = osg::computeLocalToWorld(nodepaths[0]);

        osg::Vec3d position = worldMat.getTrans();
        if (!isFirstPerson())
        {
            position.z() += mHeight * mHeightScale;

            // We subtract 10.f here and add it within focalPointOffset in order to avoid camera clipping through ceiling.
            // Needed because character's head can be a bit higher than collision area.
            position.z() -= 10.f;

            position += getFocalPointOffset() + mFocalPointAdjustment;
        }
        return position;
    }

    osg::Vec3d Camera::getFocalPointOffset() const
    {
        osg::Vec3d offset(0, 0, 10.f);
        if (mThirdPersonMode == ThirdPersonViewMode::OverShoulder && !mPreviewMode && !mVanity.enabled)
        {
            float horizontalOffset = mOverShoulderOffset.x() * (1.f - mSmoothTransitionToCombatMode);
            float verticalOffset = mSmoothTransitionToCombatMode * 15.f + (1.f - mSmoothTransitionToCombatMode) * mOverShoulderOffset.y();

            offset.x() += horizontalOffset * cos(getYaw());
            offset.y() += horizontalOffset * sin(getYaw());
            offset.z() += verticalOffset;
        }
        return offset;
    }

    void Camera::getPosition(osg::Vec3d &focal, osg::Vec3d &camera) const
    {
        focal = getFocalPoint();
        osg::Vec3d offset(0,0,0);
        if (!isFirstPerson())
        {
            osg::Quat orient =  osg::Quat(getPitch(), osg::Vec3d(1,0,0)) * osg::Quat(getYaw(), osg::Vec3d(0,0,1));
            offset = orient * osg::Vec3d(0.f, -mCameraDistance, 0.f);
        }
        camera = focal + offset;
    }

    void Camera::updateCamera(osg::Camera *cam)
    {
        if (mTrackingPtr.isEmpty())
            return;

        osg::Vec3d focal, position;
        getPosition(focal, position);

        osg::Quat orient =  osg::Quat(getPitch(), osg::Vec3d(1,0,0)) * osg::Quat(getYaw(), osg::Vec3d(0,0,1));
        osg::Vec3d forward = orient * osg::Vec3d(0,1,0);
        osg::Vec3d up = orient * osg::Vec3d(0,0,1);

        cam->setViewMatrixAsLookAt(position, position + forward, up);
    }

    void Camera::reset()
    {
        togglePreviewMode(false);
        toggleVanityMode(false);
        if (!mFirstPersonView)
            toggleViewMode();
    }

    void Camera::rotateCamera(float pitch, float yaw, bool adjust)
    {
        if (adjust)
        {
            pitch += getPitch();
            yaw += getYaw();
        }
        setYaw(yaw);
        setPitch(pitch);
    }

    void Camera::attachTo(const MWWorld::Ptr &ptr)
    {
        mTrackingPtr = ptr;
    }

    void Camera::update(float duration, bool paused)
    {
        if (mAnimation->upperBodyReady())
        {
            // Now process the view changes we queued earlier
            if (mVanityToggleQueued)
            {
                toggleVanityMode(mVanityToggleQueuedValue);
                mVanityToggleQueued = false;
            }
            if (mViewModeToggleQueued)
            {

                togglePreviewMode(false);
                toggleViewMode();
                mViewModeToggleQueued = false;
            }
        }

        if (paused)
            return;

        // only show the crosshair in game mode
        MWBase::WindowManager *wm = MWBase::Environment::get().getWindowManager();
        wm->showCrosshair(!wm->isGuiMode() && !mVanity.enabled && !mPreviewMode
                          && (mFirstPersonView || mThirdPersonMode != ThirdPersonViewMode::Standard));

        if(mVanity.enabled)
        {
            rotateCamera(0.f, osg::DegreesToRadians(3.f * duration), true);
        }

        updateSmoothTransitionToCombatMode(duration);
    }

    void Camera::setOverShoulderOffset(float horizontal, float vertical)
    {
        mOverShoulderOffset = osg::Vec2f(horizontal, vertical);
    }

    void Camera::updateSmoothTransitionToCombatMode(float duration)
    {
        bool combatMode = true;
        if (mTrackingPtr.getClass().isActor())
            combatMode = mTrackingPtr.getClass().getCreatureStats(mTrackingPtr).getDrawState() != MWMechanics::DrawState_Nothing;
        float speed = ((combatMode ? 1.f : 0.f) - mSmoothTransitionToCombatMode) * 5;
        if (speed != 0)
            speed += speed > 0 ? 1 : -1;

        mSmoothTransitionToCombatMode += speed * duration;
        if (mSmoothTransitionToCombatMode > 1)
            mSmoothTransitionToCombatMode = 1;
        if (mSmoothTransitionToCombatMode < 0)
            mSmoothTransitionToCombatMode = 0;
    }

    void Camera::toggleViewMode(bool force)
    {
        // Changing the view will stop all playing animations, so if we are playing
        // anything important, queue the view change for later
        if (!mAnimation->upperBodyReady() && !force)
        {
            mViewModeToggleQueued = true;
            return;
        }
        else
            mViewModeToggleQueued = false;

        if (mTrackingPtr.getClass().isActor())
            mTrackingPtr.getClass().getCreatureStats(mTrackingPtr).setSideMovementAngle(0);

        mFirstPersonView = !mFirstPersonView;
        processViewChange();
    }
    
    void Camera::allowVanityMode(bool allow)
    {
        if (!allow && mVanity.enabled)
            toggleVanityMode(false);
        mVanity.allowed = allow;
    }

    bool Camera::toggleVanityMode(bool enable)
    {
        // Changing the view will stop all playing animations, so if we are playing
        // anything important, queue the view change for later
        if (mFirstPersonView && !mAnimation->upperBodyReady())
        {
            mVanityToggleQueued = true;
            mVanityToggleQueuedValue = enable;
            return false;
        }

        if(!mVanity.allowed && enable)
            return false;

        if(mVanity.enabled == enable)
            return true;
        mVanity.enabled = enable;

        processViewChange();

        float offset = mPreviewCam.offset;

        if (mVanity.enabled) {
            setPitch(osg::DegreesToRadians(-30.f));
            mMainCam.offset = mCameraDistance;
        } else {
            offset = mMainCam.offset;
        }

        mCameraDistance = offset;

        return true;
    }

    void Camera::togglePreviewMode(bool enable)
    {
        if (mFirstPersonView && !mAnimation->upperBodyReady())
            return;

        if(mPreviewMode == enable)
            return;

        mPreviewMode = enable;
        processViewChange();

        float offset = mCameraDistance;
        if (mPreviewMode) {
            mMainCam.offset = offset;
            offset = mPreviewCam.offset;
        } else {
            mPreviewCam.offset = offset;
            offset = mMainCam.offset;
        }

        mCameraDistance = offset;
    }

    void Camera::setSneakOffset(float offset)
    {
        mAnimation->setFirstPersonOffset(osg::Vec3f(0,0,-offset));
    }

    float Camera::getYaw() const
    {
        if(mVanity.enabled || mPreviewMode)
            return mPreviewCam.yaw;
        return mMainCam.yaw;
    }

    void Camera::setYaw(float angle)
    {
        if (angle > osg::PI) {
            angle -= osg::PI*2;
        } else if (angle < -osg::PI) {
            angle += osg::PI*2;
        }
        if (mVanity.enabled || mPreviewMode) {
            mPreviewCam.yaw = angle;
        } else {
            mMainCam.yaw = angle;
        }
    }

    float Camera::getPitch() const
    {
        if (mVanity.enabled || mPreviewMode) {
            return mPreviewCam.pitch;
        }
        return mMainCam.pitch;
    }

    void Camera::setPitch(float angle)
    {
        const float epsilon = 0.000001f;
        float limit = osg::PI_2 - epsilon;
        if(mPreviewMode)
            limit /= 2;

        if(angle > limit)
            angle = limit;
        else if(angle < -limit)
            angle = -limit;

        if (mVanity.enabled || mPreviewMode) {
            mPreviewCam.pitch = angle;
        } else {
            mMainCam.pitch = angle;
        }
    }

    float Camera::getCameraDistance() const
    {
        if (isFirstPerson())
            return 0.f;
        return mCameraDistance;
    }

    void Camera::setBaseCameraDistance(float dist, bool adjust)
    {
        if(mFirstPersonView && !mPreviewMode && !mVanity.enabled)
            return;

        mIsNearest = false;

        if (adjust)
        {
            if (mVanity.enabled || mPreviewMode)
                dist += mCameraDistance;
            else
                dist += std::min(mCameraDistance - getCameraDistanceCorrection(), mBaseCameraDistance);
        }


        if (dist >= mFurthest)
            dist = mFurthest;
        else if (dist <= mNearest)
        {
            dist = mNearest;
            mIsNearest = true;
        }

        if (mVanity.enabled || mPreviewMode)
            mPreviewCam.offset = dist;
        else if (!mFirstPersonView)
            mBaseCameraDistance = dist;
        setCameraDistance();
    }

    void Camera::setCameraDistance(float dist, bool adjust)
    {
        if(mFirstPersonView && !mPreviewMode && !mVanity.enabled)
            return;

        if (adjust) dist += mCameraDistance;

        if (dist >= mFurthest)
            dist = mFurthest;
        else if (dist < 10.f)
            dist = 10.f;
        mCameraDistance = dist;
    }

    float Camera::getCameraDistanceCorrection() const
    {
        return mThirdPersonMode != ThirdPersonViewMode::Standard ? std::max(-getPitch(), 0.f) * 50.f : 0;
    }

    void Camera::setCameraDistance()
    {
        if (mVanity.enabled || mPreviewMode)
            mCameraDistance = mPreviewCam.offset;
        else if (!mFirstPersonView)
            mCameraDistance = mBaseCameraDistance + getCameraDistanceCorrection();
        mFocalPointAdjustment = osg::Vec3d();
    }

    void Camera::setAnimation(NpcAnimation *anim)
    {
        mAnimation = anim;

        processViewChange();
    }

    void Camera::processViewChange()
    {
        if(isFirstPerson())
        {
            mAnimation->setViewMode(NpcAnimation::VM_FirstPerson);
            mTrackingNode = mAnimation->getNode("Camera");
            if (!mTrackingNode)
                mTrackingNode = mAnimation->getNode("Head");
            mHeightScale = 1.f;
        }
        else
        {
            mAnimation->setViewMode(NpcAnimation::VM_Normal);
            SceneUtil::PositionAttitudeTransform* transform = mTrackingPtr.getRefData().getBaseNode();
            mTrackingNode = transform;
            if (transform)
                mHeightScale = transform->getScale().z();
            else
                mHeightScale = 1.f;
        }
        rotateCamera(getPitch(), getYaw(), false);
    }

    bool Camera::isVanityOrPreviewModeEnabled() const
    {
        return mPreviewMode || mVanity.enabled;
    }

    bool Camera::isNearest() const
    {
        return mIsNearest;
    }
}
