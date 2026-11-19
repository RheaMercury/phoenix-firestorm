/**
 * @file fsgddcommon.h
 * @brief Pieces every GPU-driven pass shares (fsgpudriven*.cpp): the indirect command, the matrix table,
 * buffer uploads, and the verify and statistics helpers
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

#ifndef FS_GDDCOMMON_H
#define FS_GDDCOMMON_H

#include "fsgddarena.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglstates.h"
#include "v4math.h"
#include "m4math.h"

#include "glm/gtc/type_ptr.hpp"

#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// DrawElementsIndirectCommand
struct FSGDDDrawCommand
{
    GLuint  mCount;
    GLuint  mInstanceCount;
    GLuint  mFirstIndex;
    GLint   mBaseVertex;
    GLuint  mBaseInstance;
};

// uvec2, std430 stride 8: the texture lists of the legacy, alpha and shadow alpha passes
struct FSGDDTexEntry
{
    U32 mX;         // header: minimum alpha (float bits); handle: low 32 bits
    U32 mY;         // header: texture count; handle: high 32 bits
};

// The matrices one multi-draw reads from its storage buffer: model matrices, composed the way
// LLRenderPass::applyModelMatrix and LLRender::syncMatrices compose them, and texture matrices, each stored once
// per pass. Consecutive batches mostly share their matrices, so the last lookup of each kind is kept too.
class FSGDDMatrixTable
{
public:
    void clear()
    {
        mMatrices.clear();
        mModelIndex.clear();
        mTexIndex.clear();
        mLastModelIndex = U32_MAX;
        mLastTexIndex = U32_MAX;
    }

    // The deferred passes: modelview, modelview projection and the normal matrix (its upper 3x3), in that order.
    U32 modelIndex(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view)
    {
        return lookupModel(model, proj, view, true);
    }

    // The shadow passes: the modelview projection only.
    U32 mvpIndex(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view)
    {
        return lookupModel(model, proj, view, false);
    }

    // 0 for none: the identity, what MM_TEXTURE0 holds outside the stock batch code that loads one.
    U32 textureIndex(const LLMatrix4* texture_matrix)
    {
        if (!texture_matrix)
        {
            return 0;
        }
        if (mLastTexIndex != U32_MAX && texture_matrix == mLastTex)
        {
            return mLastTexIndex;
        }
        mLastTex = texture_matrix;
        auto it = mTexIndex.find(texture_matrix);
        if (it != mTexIndex.end())
        {
            mLastTexIndex = it->second;
            return it->second;
        }
        const U32 index = push(glm::make_mat4((const F32*)texture_matrix->mMatrix));
        mTexIndex.emplace(texture_matrix, index);
        mLastTexIndex = index;
        return index;
    }

    // Any other matrix the pass wants in the same buffer; its index.
    U32 push(const glm::mat4& matrix)
    {
        const U32 index = (U32)mMatrices.size();
        mMatrices.push_back(matrix);
        return index;
    }

    const glm::mat4* data() const   { return mMatrices.data(); }
    size_t size() const             { return mMatrices.size(); }
    size_t bytes() const            { return mMatrices.size() * sizeof(glm::mat4); }

private:
    U32 lookupModel(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view, bool full)
    {
        if (mLastModelIndex != U32_MAX && model == mLastModel)
        {
            return mLastModelIndex;
        }
        mLastModel = model;
        auto it = mModelIndex.find(model);
        if (it != mModelIndex.end())
        {
            mLastModelIndex = it->second;
            return it->second;
        }
        glm::mat4 modelview = view;
        if (model)
        {
            modelview *= glm::make_mat4((const F32*)model->mMatrix);
        }
        const U32 index = (U32)mMatrices.size();
        if (full)
        {
            mMatrices.push_back(modelview);
            mMatrices.push_back(proj * modelview);
            mMatrices.push_back(glm::transpose(glm::inverse(modelview)));
        }
        else
        {
            mMatrices.push_back(proj * modelview);
        }
        mModelIndex.emplace(model, index);
        mLastModelIndex = index;
        return index;
    }

    std::vector<glm::mat4> mMatrices;
    std::unordered_map<const LLMatrix4*, U32> mModelIndex;
    std::unordered_map<const LLMatrix4*, U32> mTexIndex;
    const LLMatrix4* mLastModel = nullptr;
    U32 mLastModelIndex = U32_MAX;      // U32_MAX: none yet this pass
    const LLMatrix4* mLastTex = nullptr;
    U32 mLastTexIndex = U32_MAX;
};

// New storage for a per multi-draw buffer: the previous pass's draws may still read the old. Never empty.
inline void fsGDDUpload(GLuint& buffer, GLenum target, const void* data, size_t bytes)
{
    if (!buffer)
    {
        glGenBuffers(1, &buffer);
    }
    glBindBuffer(target, buffer);
    glBufferData(target, llmax(bytes, (size_t)16), bytes ? data : nullptr, GL_STREAM_DRAW);
}

// Deletes the buffers that exist and zeroes every name.
inline void fsGDDDeleteBuffers(std::initializer_list<GLuint*> buffers)
{
    for (GLuint* buffer : buffers)
    {
        if (*buffer)
        {
            glDeleteBuffers(1, buffer);
            *buffer = 0;
        }
    }
}

inline void fsGDDDeleteQuery(GLuint& query)
{
    if (query)
    {
        glDeleteQueries(1, &query);
        query = 0;
    }
}

// True at most once every five seconds per timer: how often a verify warning may repeat.
inline bool fsGDDWarnDue(LLFrameTimer& timer)
{
    if (timer.getElapsedTimeF32() > 5.f)
    {
        timer.reset();
        return true;
    }
    return false;
}

// Verify: warns (at most every five seconds) of a GL error left by the pass.
inline void fsGDDCheckGLError(LLFrameTimer& timer, const char* setting, const std::string& pass)
{
    GLenum error = glGetError();
    if (error != GL_NO_ERROR && fsGDDWarnDue(timer))
    {
        LL_WARNS("Rendering") << setting << ": GL error 0x" << std::hex << error << std::dec
                              << " after the GPU-driven " << pass << " pass" << LL_ENDL;
    }
}

// Verify: reads back the arena copies of a couple of the pass's vertex buffers and compares them with their
// sources. vb_of maps an element of the range to its LLVertexBuffer. Returns the mismatches found.
// peek, not find: a verify pass must not copy, evict or touch entries.
template <typename Range, typename VBOf>
U32 fsGDDVerifyArenaCopies(FSGDDArena& arena, const Range& range, VBOf vb_of, const char* setting)
{
    static LLFrameTimer warn_timer;
    U32 mismatches = 0;
    U32 checked = 0;
    for (const auto& element : range)
    {
        LLVertexBuffer* vb = vb_of(element);
        FSGDDArenaEntry* found = arena.peek(vb);
        if (found && !found->mVerified)
        {
            if (!arena.verifyEntry(vb, *found))
            {
                ++mismatches;
                if (fsGDDWarnDue(warn_timer))
                {
                    LL_WARNS("Rendering") << setting << ": the arena copy of a vertex buffer ("
                                          << found->mNumVerts << " vertices, " << found->mNumIndices
                                          << " indices) differs from the buffer" << LL_ENDL;
                }
            }
            if (++checked >= 2)
            {
                break;
            }
        }
    }
    return mismatches;
}

// Verify: draws the stock path again (draw()) with depth writes off and depth test GL_LESS under a
// GL_SAMPLES_PASSED query, so a sample passes only where the stock geometry is nearer than what the multi-draws
// left. Colour passes turn colour writes off behind gGL's back (put back after); shadow passes, which write no
// colour, offset the stock depth so that only casters clearly nearer count. Stalls on the result.
template <typename Draw>
GLuint fsGDDNearerSamples(GLuint& query, bool shadow, Draw draw)
{
    if (!query)
    {
        glGenQueries(1, &query);
    }
    GLfloat factor = 0.f;
    GLfloat units = 0.f;
    if (shadow)
    {
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &factor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &units);
    }
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LESS);
        LLGLEnable offset(shadow ? GL_POLYGON_OFFSET_FILL : 0);
        GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        if (shadow)
        {
            glPolygonOffset(1.f, 4.f);
        }
        else
        {
            glGetBooleanv(GL_COLOR_WRITEMASK, mask);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        }
        glBeginQuery(GL_SAMPLES_PASSED, query);
        draw();
        glEndQuery(GL_SAMPLES_PASSED);
        if (!shadow)
        {
            glColorMask(mask[0], mask[1], mask[2], mask[3]);
        }
    }
    if (shadow)
    {
        glPolygonOffset(factor, units);
    }
    GLuint samples = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);
    return samples;
}

// Statistics: "name count, name count" for the non-zero counters (zeroed as they are read), "none" without any.
template <typename Count, typename NameOf>
std::string fsGDDCountList(Count* counts, U32 n, NameOf name_of)
{
    std::ostringstream out;
    for (U32 r = 0; r < n; ++r)
    {
        if (counts[r])
        {
            out << (out.tellp() ? ", " : "") << name_of(r) << " " << counts[r];
            counts[r] = 0;
        }
    }
    return out.tellp() ? out.str() : std::string("none");
}

// Statistics: "N MB vertices, N MB indices (N MB allocated), N buffers, N copies" for an arena.
inline std::string fsGDDArenaSummary(const FSGDDArena& arena)
{
    std::ostringstream out;
    out << arena.vertexMB() << " MB vertices, " << arena.indexMB() << " MB indices (" << arena.allocatedMB()
        << " MB allocated), " << arena.entries() << " buffers, " << arena.uploads() << " copies";
    return out.str();
}

inline U32 fsGDDPopcount4(U32 mask)
{
    return (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1) + ((mask >> 3) & 1);
}

// The single pass shadow cascades of a four bit set, as the fs_shadow_cascade vec4 lists them; their count.
inline U32 fsGDDCascadeList(U32 mask, F32 cascades[4])
{
    cascades[0] = cascades[1] = cascades[2] = cascades[3] = 0.f;
    U32 n = 0;
    for (U32 c = 0; c < 4; ++c)
    {
        if (mask & (1u << c))
        {
            cascades[n++] = (F32)c;
        }
    }
    return n;
}

#endif // FS_GDDCOMMON_H
