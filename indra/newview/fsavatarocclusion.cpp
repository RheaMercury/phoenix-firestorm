/**
 * @file fsavatarocclusion.cpp
 * @brief Soft ambient occlusion around avatars from capsules on their skeletons (FSRenderAvatarOcclusion)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsavatarocclusion.h"

#include "llcharacter.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <algorithm>
#include <vector>

extern bool gCubeSnapshot;

namespace
{
    // One capsule: from joint a to joint b, radius in meters for an avatar of the default
    // proportions (scaled by the avatar's own, see build_avatar). A little slimmer than a body, so
    // the avatar's own surface lies outside its capsules and doesn't darken itself.
    struct CapsuleDef
    {
        const char* mA;
        const char* mB;
        F32 mRadius;
    };

    const CapsuleDef CAPSULES[FSAvatarOcclusion::CAPSULES_PER_AVATAR] =
    {
        { "mPelvis",        "mTorso",       0.14f },  // hips
        { "mTorso",         "mChest",       0.13f },  // waist
        { "mChest",         "mNeck",        0.12f },  // chest and shoulders
        { "mHead",          "mSkull",       0.095f }, // head
        { "mShoulderLeft",  "mElbowLeft",   0.05f },  // upper arms
        { "mShoulderRight", "mElbowRight",  0.05f },
        { "mElbowLeft",     "mWristLeft",   0.04f },  // forearms
        { "mElbowRight",    "mWristRight",  0.04f },
        { "mHipLeft",       "mKneeLeft",    0.075f }, // thighs
        { "mHipRight",      "mKneeRight",   0.075f },
        { "mKneeLeft",      "mAnkleLeft",   0.055f }, // shins
        { "mKneeRight",     "mAnkleRight",  0.055f },
        { "mAnkleLeft",     "mFootLeft",    0.045f }, // feet
        { "mAnkleRight",    "mFootRight",   0.045f },
    };

    // Pelvis to head of the default avatar shape, for the radius scale
    constexpr F32 DEFAULT_PELVIS_TO_HEAD = 0.616f;
    // How far around its capsules an avatar darkens anything, at the default shape
    constexpr F32 REACH = 1.0f;

    // The FSAvatarCapsules block of fsAvatarOcclusionF.glsl (std140, vec4 arrays): capsule starts
    // (xyz, radius), capsule ends (xyz, unused), per avatar bounds (centre, radius)
    constexpr U32 TOTAL_CAPSULES = FSAvatarOcclusion::MAX_AVATARS * FSAvatarOcclusion::CAPSULES_PER_AVATAR;
    constexpr U32 BLOCK_VEC4 = TOTAL_CAPSULES * 2 + FSAvatarOcclusion::MAX_AVATARS;

    std::vector<LLVector4> sBlock;
    GLuint sUBO = 0;

    LLVector3 to_eye(const glm::mat4& mv, const LLVector3& p)
    {
        const glm::vec4 e = mv * glm::vec4(p.mV[VX], p.mV[VY], p.mV[VZ], 1.f);
        return LLVector3(e.x, e.y, e.z);
    }

    // The capsules of one avatar into slot `slot` of the block. False when its skeleton lacks the
    // pelvis or head (the slot is then left empty).
    bool build_avatar(LLVOAvatar* avatar, U32 slot, const glm::mat4& mv)
    {
        if (!avatar->mPelvisp || !avatar->mHeadp)
        {
            return false;
        }
        const LLVector3 pelvis = avatar->mPelvisp->getWorldPosition();
        const F32 scale = llclamp(dist_vec(avatar->mHeadp->getWorldPosition(), pelvis) / DEFAULT_PELVIS_TO_HEAD, 0.25f, 4.f);

        const LLVector3 centre = to_eye(mv, pelvis);
        F32 extent = 0.f;
        for (U32 i = 0; i < FSAvatarOcclusion::CAPSULES_PER_AVATAR; ++i)
        {
            const U32 index = slot * FSAvatarOcclusion::CAPSULES_PER_AVATAR + i;
            LLVector4& a = sBlock[index];
            LLVector4& b = sBlock[TOTAL_CAPSULES + index];
            LLJoint* ja = avatar->getJoint(CAPSULES[i].mA);
            LLJoint* jb = avatar->getJoint(CAPSULES[i].mB);
            if (!ja || !jb)
            {
                a.set(0.f, 0.f, 0.f, 0.f); // radius 0: skipped by the shader
                b.set(0.f, 0.f, 0.f, 0.f);
                continue;
            }
            const LLVector3 pa = to_eye(mv, ja->getWorldPosition());
            const LLVector3 pb = to_eye(mv, jb->getWorldPosition());
            const F32 radius = CAPSULES[i].mRadius * scale;
            a.set(pa.mV[VX], pa.mV[VY], pa.mV[VZ], radius);
            b.set(pb.mV[VX], pb.mV[VY], pb.mV[VZ], 0.f);
            extent = llmax(extent, dist_vec(pa, centre) + radius, dist_vec(pb, centre) + radius);
        }
        sBlock[TOTAL_CAPSULES * 2 + slot].set(centre.mV[VX], centre.mV[VY], centre.mV[VZ], extent + REACH * scale);
        return true;
    }

    // The nearest avatars within range into the block, nearest first. Returns how many.
    U32 build(const glm::mat4& mv)
    {
        static LLCachedControl<S32> max_avatars(gSavedSettings, "FSRenderAvatarOcclusionMaxAvatars", 8);
        static LLCachedControl<F32> max_distance(gSavedSettings, "FSRenderAvatarOcclusionDistance", 32.f);
        const U32 wanted = (U32)llclamp((S32)max_avatars, 1, (S32)FSAvatarOcclusion::MAX_AVATARS);
        const F32 range = llclamp((F32)max_distance, 4.f, 256.f);
        const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();

        std::vector<std::pair<F32, LLVOAvatar*>> nearby;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            // Animesh can be any shape; UI avatars are not in the world; hidden avatars cast nothing
            if (!avatar || avatar->isDead() || avatar->isUIAvatar() || avatar->isControlAvatar() ||
                !avatar->isVisible() || !avatar->mPelvisp)
            {
                continue;
            }
            const F32 distance = dist_vec(avatar->mPelvisp->getWorldPosition(), camera);
            if (distance <= range)
            {
                nearby.emplace_back(distance, avatar);
            }
        }
        if (nearby.size() > wanted)
        {
            std::partial_sort(nearby.begin(), nearby.begin() + wanted, nearby.end(),
                              [](const auto& l, const auto& r) { return l.first < r.first; });
            nearby.resize(wanted);
        }

        U32 count = 0;
        for (const auto& entry : nearby)
        {
            if (build_avatar(entry.second, count, mv))
            {
                ++count;
            }
        }
        return count;
    }
}

bool FSAvatarOcclusion::enabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderAvatarOcclusion", false);
    return enabled;
}

void FSAvatarOcclusion::bind(LLGLSLShader& shader)
{
    if (!shader.mDefines.count("FS_AVATAR_OCCLUSION") || !shader.mProgramObject)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    static LLCachedControl<F32> strength(gSavedSettings, "FSRenderAvatarOcclusionStrength", 0.8f);
    static LLCachedControl<F32> shadow(gSavedSettings, "FSRenderAvatarOcclusionShadow", 0.5f);
    static LLStaticHashedString s_params("fs_cap_params");
    static LLStaticHashedString s_sky("fs_cap_sky");

    if (sBlock.size() != BLOCK_VEC4)
    {
        sBlock.assign(BLOCK_VEC4, LLVector4(0.f, 0.f, 0.f, 0.f));
    }
    if (!sUBO)
    {
        glGenBuffers(1, &sUBO);
        if (!sUBO)
        {
            return;
        }
        // Always backed, so the block has a buffer even in a view with no capsules
        glBindBuffer(GL_UNIFORM_BUFFER, sUBO);
        glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(BLOCK_VEC4 * sizeof(LLVector4)), sBlock.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    // No capsules in reflection probe captures: too rare to follow moving avatars, and their
    // ambient light would carry the darkening into the probes
    const glm::mat4 mv = get_current_modelview();
    const U32 count = (gCubeSnapshot || strength <= 0.f) ? 0 : build(mv);
    if (count > 0)
    {
        // Only the slots in use: the shader reads no further
        glBindBuffer(GL_UNIFORM_BUFFER, sUBO);
        const U32 caps = count * CAPSULES_PER_AVATAR;
        glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)(caps * sizeof(LLVector4)), sBlock.data());
        glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)(TOTAL_CAPSULES * sizeof(LLVector4)),
                        (GLsizeiptr)(caps * sizeof(LLVector4)), sBlock.data() + TOTAL_CAPSULES);
        glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)(TOTAL_CAPSULES * 2 * sizeof(LLVector4)),
                        (GLsizeiptr)(count * sizeof(LLVector4)), sBlock.data() + TOTAL_CAPSULES * 2);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    // The block's binding point (UBO_BINDING) is set once per link and per load from the binary
    // cache, in LLGLSLShader::mapUniforms: looking its index up here at every bind made the main
    // thread wait for the render thread every frame
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_BINDING, sUBO);

    // The open sky's direction in eye space, for the soft shadow it casts past the avatars
    const glm::vec4 up = mv * glm::vec4(0.f, 0.f, 1.f, 0.f);
    shader.uniform4f(s_params, (F32)count, llclamp((F32)strength, 0.f, 1.f), llclamp((F32)shadow, 0.f, 1.f), 0.f);
    shader.uniform3f(s_sky, up.x, up.y, up.z);
}

void FSAvatarOcclusion::release()
{
    if (sUBO)
    {
        glDeleteBuffers(1, &sUBO);
        sUBO = 0;
    }
    sBlock.clear();
    sBlock.shrink_to_fit();
}
