/**
 * @file fsskinpalettebuffer.cpp
 * @brief Rigged mesh joint palettes written once per frame into one uniform buffer (FSPerfRiggedPaletteBuffer)
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

#include "fsskinpalettebuffer.h"

#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llskinningutil.h"
#include "llspatialpartition.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"
#include "v3math.h"

#include <unordered_map>
#include <vector>

namespace
{
    // Block layout (std140, objectSkinV.glsl): vec4 header (skin origin, palette size), then the
    // palette, 48 bytes a matrix. Every range bound is the block's full size, so no shader read can
    // fall outside it whatever the palette's own size.
    constexpr U32 HEADER_BYTES = 16;
    constexpr U32 MATRIX_BYTES = 48;
    constexpr U32 INITIAL_BYTES = 1024 * 1024;

    bool sBuilt = false;
    GLuint sBuffer = 0;
    U32 sCapacity = 0;
    U32 sUsed = 0;
    U32 sFrame = U32_MAX;
    U32 sAlign = 0;
    U32 sBlockBytes = 0;
    U32 sBoundOffset = U32_MAX;
    U32 sGeneration = 0;                    // storages so far (FSSkinPaletteBuffer::generation)
    std::unordered_map<U32, U32> sOffsets;  // build serial -> offset, this frame
    std::vector<F32> sScratch;

    // Gives the buffer new storage of sCapacity bytes; ranges already drawn with keep the old one
    void orphan()
    {
        glBindBuffer(GL_UNIFORM_BUFFER, sBuffer);
        glBufferData(GL_UNIFORM_BUFFER, sCapacity, nullptr, GL_STREAM_DRAW);
        sUsed = 0;
        sOffsets.clear();
        sBoundOffset = U32_MAX;
        ++sGeneration;
    }

    bool ensureBuffer()
    {
        if (sBuffer)
        {
            return true;
        }
        if (!sAlign)
        {
            GLint align = 256;
            glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
            // a multiple of 16 too: the storage buffer view counts offsets in vec4s
            sAlign = ((U32)llclamp(align, 16, 1024) + 15) & ~15u;
            sBlockBytes = HEADER_BYTES + MATRIX_BYTES * LLSkinningUtil::getMaxJointCount();
        }
        glGenBuffers(1, &sBuffer);
        if (!sBuffer)
        {
            return false;
        }
        sCapacity = INITIAL_BYTES;
        orphan();
        return true;
    }
}

bool FSSkinPaletteBuffer::wanted()
{
    static LLCachedControl<bool> setting(gSavedSettings, "FSPerfRiggedPaletteBuffer", false);
    return setting;
}

void FSSkinPaletteBuffer::setShadersBuilt(bool built)
{
    sBuilt = built;
    if (!built)
    {
        release();
    }
}

bool FSSkinPaletteBuffer::active()
{
    return sBuilt;
}

U32 FSSkinPaletteBuffer::offsetOf(U32 serial, const F32* matrices, U32 count, const LLVector3& skin_origin)
{
    if (!ensureBuffer())
    {
        return U32_MAX;
    }

    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != sFrame)
    {
        sFrame = frame;
        orphan();
    }

    if (serial != 0)
    {
        auto it = sOffsets.find(serial);
        if (it != sOffsets.end())
        {
            return it->second;
        }
    }

    count = llmin(count, LLSkinningUtil::getMaxJointCount());
    const U32 bytes = HEADER_BYTES + MATRIX_BYTES * count;
    U32 offset = (sUsed + sAlign - 1) / sAlign * sAlign;
    if (offset + sBlockBytes > sCapacity)
    {
        // Full: start over in storage twice the size (draws already recorded keep theirs)
        sCapacity = llmax(sCapacity * 2, sBlockBytes + sAlign);
        orphan();
        offset = 0;
    }

    sScratch.resize(bytes / sizeof(F32));
    sScratch[0] = skin_origin.mV[VX];
    sScratch[1] = skin_origin.mV[VY];
    sScratch[2] = skin_origin.mV[VZ];
    sScratch[3] = (F32)count;
    memcpy(sScratch.data() + 4, matrices, MATRIX_BYTES * count);

    glBindBuffer(GL_UNIFORM_BUFFER, sBuffer);
    glBufferSubData(GL_UNIFORM_BUFFER, offset, bytes, sScratch.data());
    sUsed = offset + bytes;
    if (serial != 0)
    {
        sOffsets[serial] = offset;
    }
    return offset;
}

void FSSkinPaletteBuffer::bind(U32 serial, const F32* matrices, U32 count, const LLVector3& skin_origin)
{
    const U32 offset = offsetOf(serial, matrices, count, skin_origin);
    if (offset == U32_MAX)
    {
        return;
    }
    if (offset != sBoundOffset)
    {
        glBindBufferRange(GL_UNIFORM_BUFFER, LLGLSLShader::UB_FS_SKIN_PALETTE, sBuffer, offset, sBlockBytes);
        sBoundOffset = offset;
    }
}

U32 FSSkinPaletteBuffer::generation()
{
    return sGeneration;
}

U32 FSSkinPaletteBuffer::glName()
{
    return sBuffer;
}

bool FSSkinPaletteBuffer::drawable(LLDrawInfo& params)
{
    if (params.mAvatar.isNull() || params.mSkinInfo.isNull())
    {
        return false;
    }
    return !params.mAvatar->updateSkinInfoMatrixPalette(params.mSkinInfo.get()).mMatrixPalette.empty();
}

bool FSSkinPaletteBuffer::place(const std::vector<LLDrawInfo*>& drawn, const std::vector<LLDrawInfo*>& stock,
                                std::vector<U32>& offsets, U32& generation)
{
    // the palettes once (the entries stay put: unordered_map nodes, built at most once a frame)
    static std::vector<const LLVOAvatar::MatrixPaletteCache*> palettes;
    palettes.clear();
    for (LLDrawInfo* params : drawn)
    {
        if (!params || !drawable(*params))
        {   // the caller only gives drawable ones
            return false;
        }
        palettes.push_back(&params->mAvatar->updateSkinInfoMatrixPalette(params->mSkinInfo.get()));
    }
    const size_t drawn_count = palettes.size();
    for (LLDrawInfo* params : stock)
    {
        if (params && drawable(*params))
        {
            palettes.push_back(&params->mAvatar->updateSkinInfoMatrixPalette(params->mSkinInfo.get()));
        }
    }

    offsets.resize(drawn_count);
    for (U32 attempt = 0; attempt < 4; ++attempt)
    {
        const U32 start = sGeneration;
        for (size_t i = 0; i < palettes.size(); ++i)
        {
            const LLVOAvatar::MatrixPaletteCache& mpc = *palettes[i];
            // the count the stock upload passes (fs_upload_palette)
            const U32 offset = offsetOf(mpc.mFSBuildSerial, mpc.mGLMp.data(), (U32)mpc.mMatrixPalette.size(), mpc.mSkinOrigin);
            if (offset == U32_MAX)
            {
                return false;
            }
            if (i < drawn_count)
            {
                offsets[i] = offset / 16;
            }
        }
        if (start == sGeneration)
        {
            generation = start;
            return sBuffer != 0;
        }
    }
    return false;
}

void FSSkinPaletteBuffer::release()
{
    if (sBuffer)
    {
        glDeleteBuffers(1, &sBuffer);
        sBuffer = 0;
    }
    sCapacity = 0;
    sUsed = 0;
    sFrame = U32_MAX;
    sBoundOffset = U32_MAX;
    sOffsets.clear();
    ++sGeneration;
}
