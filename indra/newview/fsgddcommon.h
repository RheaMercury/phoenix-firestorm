/**
 * @file fsgddcommon.h
 * @brief Pieces every GPU-driven pass shares (fsgpudriven*.cpp): the indirect command, the matrix table,
 * buffer uploads (and the upload ring, fsgddcommon.cpp), and the verify and statistics helpers
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
#include "llfasttimer.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglstates.h"
#include "llviewercontrol.h"
#include "v4math.h"
#include "m4math.h"

#include "glm/gtc/type_ptr.hpp"

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

// Where a multi-draw input buffer (commands, records, matrices, materials, texture lists) went at its last upload.
// By default every upload gives the pass's own buffer new storage (glBufferData), so the previous pass's draws keep
// reading the old. With FSPerfGPUDrivenUploadRing the data goes into a slice of a few large buffers that take turns
// over three frames instead (fsgddcommon.cpp): mapped persistently where OpenGL 4.4 allows it and the main thread
// makes the GL calls itself, written with glBufferSubData while the render thread replays them. Main thread.
class FSGDDBuffer
{
public:
    FSGDDBuffer() = default;
    FSGDDBuffer(const FSGDDBuffer&) = delete;
    FSGDDBuffer& operator=(const FSGDDBuffer&) = delete;

    // New storage holding data, bound to target afterwards (as glBindBuffer). Never empty.
    void upload(GLenum target, const void* data, size_t bytes)  { allocate(target, bytes, data, bytes); }
    // New storage of capacity bytes, the first bytes of it data, bound to target afterwards.
    void allocate(GLenum target, size_t capacity, const void* data, size_t bytes);
    // Writes into the storage the last allocate made, offset bytes in, bound to target afterwards. False, with
    // nothing written, when that storage can't take it (too small, or a ring slice of an earlier frame).
    bool writeAt(GLenum target, size_t offset, const void* data, size_t bytes);

    void bind(GLenum target) const  { glBindBuffer(target, mName); }
    // The whole storage on an indexed binding: glBindBufferBase of the own buffer as before, a range of a ring buffer
    void bindBase(GLenum target, GLuint index) const;
    // A byte offset into the data as the bound buffer sees it: indirect command and attribute offsets
    const void* at(size_t offset) const { return (const void*)(mOffset + offset); }

    // Deletes the own buffer; ring slices belong to the ring.
    void release();

private:
    GLuint mOwn = 0;            // the own buffer, given new storage at each upload outside the ring
    GLuint mName = 0;           // where the data is: mOwn or a ring buffer
    size_t mOffset = 0;         // of the data in mName
    size_t mCapacity = 0;
    U8* mMapped = nullptr;      // persistent ring: mOffset, mapped
    U32 mRingFrame = 0;         // ring slice: the frame and ring generation it belongs to
    U32 mRingGeneration = 0;
};

// The matrices a table holds, its lookups, and the buffer it goes to
struct FSGDDMatrixStore
{
    void clear()
    {
        mMatrices.clear();
        mModelIndex.clear();
        mTexIndex.clear();
        mLastModel = nullptr;
        mLastModelIndex = U32_MAX;
        mLastTex = nullptr;
        mLastTexIndex = U32_MAX;
        mUploaded = 0;
    }

    std::vector<glm::mat4> mMatrices;
    std::unordered_map<const LLMatrix4*, U32> mModelIndex;
    std::unordered_map<const LLMatrix4*, U32> mTexIndex;
    const LLMatrix4* mLastModel = nullptr;
    U32 mLastModelIndex = U32_MAX;      // U32_MAX: none yet
    const LLMatrix4* mLastTex = nullptr;
    U32 mLastTexIndex = U32_MAX;
    FSGDDBuffer mBuffer;
    size_t mUploaded = 0;               // shared tables: the matrices mBuffer already holds
};

// FSPerfGPUDrivenSharedMatrices: the frame's table for this projection, view and kind (full: modelview, modelview
// projection and normal matrix per model; otherwise the modelview projection only), as the passes before left it;
// a new one, holding the identity at 0, for the first pass. fsgddcommon.cpp.
FSGDDMatrixStore* fsGDDSharedMatrixStore(const glm::mat4& proj, const glm::mat4& view, bool full);
// The matrix lookups, for the FSGPUDrivenUpload statistics line
extern U64 gFSGDDStatModelsComputed;
extern U64 gFSGDDStatModelsFound;

// The matrices one multi-draw reads from its storage buffer: model matrices, composed the way
// LLRenderPass::applyModelMatrix and LLRender::syncMatrices compose them, and texture matrices, each stored once
// per pass. Consecutive batches mostly share their matrices, so the last lookup of each kind is kept too.
// With FSPerfGPUDrivenSharedMatrices every pass of a frame that draws with the same projection and view (the
// camera's opaque, rigged and alpha passes; the passes of one shadow map) shares one table instead: a model's
// matrices, the inverse for its normal matrix included, are computed and uploaded once, and a pass uploads only
// what the passes before it didn't add. The same math either way, so the multi-draws stay depth identical to the
// stock draws.
class FSGDDMatrixTable
{
public:
    FSGDDMatrixTable() = default;
    FSGDDMatrixTable(const FSGDDMatrixTable&) = delete;
    FSGDDMatrixTable& operator=(const FSGDDMatrixTable&) = delete;

    // Starts a pass. full: the pass looks models up with modelIndex, else with mvpIndex. identity: it wants the
    // identity at 0 (texture matrix 0); a shared table always has it.
    void begin(const glm::mat4& proj, const glm::mat4& view, bool full, bool identity)
    {
        static LLCachedControl<bool> shared(gSavedSettings, "FSPerfGPUDrivenSharedMatrices", false);
        mStore = shared ? fsGDDSharedMatrixStore(proj, view, full) : nullptr;
        mShared = mStore != nullptr;
        if (!mStore)
        {
            mStore = &mOwn;
            mOwn.clear();
            if (identity)
            {
                push(glm::mat4(1.f));
            }
        }
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
        FSGDDMatrixStore& s = *mStore;
        if (s.mLastTexIndex != U32_MAX && texture_matrix == s.mLastTex)
        {
            return s.mLastTexIndex;
        }
        s.mLastTex = texture_matrix;
        auto it = s.mTexIndex.find(texture_matrix);
        if (it != s.mTexIndex.end())
        {
            s.mLastTexIndex = it->second;
            return it->second;
        }
        const U32 index = push(glm::make_mat4((const F32*)texture_matrix->mMatrix));
        s.mTexIndex.emplace(texture_matrix, index);
        s.mLastTexIndex = index;
        return index;
    }

    // Any other matrix the pass wants in the same buffer; its index.
    U32 push(const glm::mat4& matrix)
    {
        const U32 index = (U32)mStore->mMatrices.size();
        mStore->mMatrices.push_back(matrix);
        return index;
    }

    // To the GPU: the whole table (an own one), or what the shared table's buffer doesn't hold yet. Leaves the
    // buffer bound to GL_SHADER_STORAGE_BUFFER when it uploaded anything.
    void upload()
    {
        FSGDDMatrixStore& s = *mStore;
        const size_t bytes = s.mMatrices.size() * sizeof(glm::mat4);
        if (!mShared)
        {
            s.mBuffer.upload(GL_SHADER_STORAGE_BUFFER, s.mMatrices.data(), bytes);
            return;
        }
        const size_t count = s.mMatrices.size();
        if (s.mUploaded == count)
        {
            return;
        }
        const size_t done = s.mUploaded * sizeof(glm::mat4);
        if (!s.mUploaded || !s.mBuffer.writeAt(GL_SHADER_STORAGE_BUFFER, done, (const U8*)s.mMatrices.data() + done, bytes - done))
        {   // with room for the passes still to come, so they mostly only write what they add
            s.mBuffer.allocate(GL_SHADER_STORAGE_BUFFER, llmax(bytes * 2, (size_t)(64 * 1024)), s.mMatrices.data(), bytes);
        }
        s.mUploaded = count;
    }

    void bindBase(GLuint index) const   { mStore->mBuffer.bindBase(GL_SHADER_STORAGE_BUFFER, index); }
    size_t size() const                 { return mStore->mMatrices.size(); }
    // The own table's buffer; fsGDDCommonReleaseGL releases the shared ones.
    void release()                      { mOwn.mBuffer.release(); }

private:
    U32 lookupModel(const LLMatrix4* model, const glm::mat4& proj, const glm::mat4& view, bool full)
    {
        FSGDDMatrixStore& s = *mStore;
        if (s.mLastModelIndex != U32_MAX && model == s.mLastModel)
        {
            ++gFSGDDStatModelsFound;
            return s.mLastModelIndex;
        }
        s.mLastModel = model;
        auto it = s.mModelIndex.find(model);
        if (it != s.mModelIndex.end())
        {
            ++gFSGDDStatModelsFound;
            s.mLastModelIndex = it->second;
            return it->second;
        }
        ++gFSGDDStatModelsComputed;
        glm::mat4 modelview = view;
        if (model)
        {
            modelview *= glm::make_mat4((const F32*)model->mMatrix);
        }
        const U32 index = (U32)s.mMatrices.size();
        if (full)
        {
            s.mMatrices.push_back(modelview);
            s.mMatrices.push_back(proj * modelview);
            s.mMatrices.push_back(glm::transpose(glm::inverse(modelview)));
        }
        else
        {
            s.mMatrices.push_back(proj * modelview);
        }
        s.mModelIndex.emplace(model, index);
        s.mLastModelIndex = index;
        return index;
    }

    FSGDDMatrixStore mOwn;
    FSGDDMatrixStore* mStore = &mOwn;
    bool mShared = false;
};

// FSPerfGPUDrivenSharedMatrices: a new cull result is about to be drawn (LLPipeline::postSort). The shared tables
// start again from it, so a matrix freed by a group rebuild and allocated again at the same address can't find
// a stale entry.
void fsGDDNewCull();
// The ring's and the shared tables' GL objects (fsGDDReleaseGL)
void fsGDDCommonReleaseGL();

// For the render breakdown, over every GPU-driven pass: turning the batches into commands, and the uploads
extern LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD;
extern LLTrace::BlockTimerStatHandle FTM_FS_GDD_UPLOAD;

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
