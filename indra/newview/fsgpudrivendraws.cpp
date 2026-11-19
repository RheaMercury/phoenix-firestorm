/**
 * @file fsgpudrivendraws.cpp
 * @brief GPU-driven draws, first slice: opaque shadow casters as one multi-draw (FSPerfGPUDrivenDraws)
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

// genDrawInfo gives every batch (a run of faces with one texture or one texture set) a vertex buffer of
// its own, so on the stock path every opaque shadow caster costs a buffer bind, a vertex attribute setup,
// a model matrix upload and a glDrawRangeElements, once per shadow map it reaches. Merging draws that share
// a buffer (FSPerfShadowMergedBatches) can't get below one call per buffer.
//
// With FSPerfGPUDrivenDraws on, the depth only opaque casters (the pass types renderShadow draws with
// gDeferredShadowProgram, not rigged) come out of a position arena instead: one GL buffer holding a copy of
// the positions of every caster's vertex buffer, and one holding a copy of its 16 bit indices. A buffer is
// copied in on the GPU (glCopyBufferSubData, nothing goes through the CPU) the first time it casts, and again
// whenever its content serial (LLVertexBuffer::getFSContentSerial, bumped on every upload) changes. Entries
// that no shadow pass used for a while are dropped. Each caster then is one 20 byte indirect command
// (index range offset into the arena, base vertex of its copy, and as base instance its own index) plus a
// record per instance naming its matrix, the model view projection the stock path would have computed for it,
// and (single pass) its cascade. The vertex shader reads the records as an instanced attribute with divisor 1
// stepped from the command's base instance, which is what gl_DrawIDARB would give without needing
// GL_ARB_shader_draw_parameters; it never reads gl_InstanceID, which some drivers offset by the base instance.
// A whole shadow pass is one glMultiDrawElementsIndirect; with FSPerfShadowSinglePass, one per set of
// cascades, each command instanced once per cascade like the stock single pass draws.
//
// Anything that can't come from the arena (32 bit indices, a buffer still mapped, the per frame copy budget
// or the arena size limit reached) stays in the list and is drawn by the stock loop. Rigged, alpha masked,
// GLTF, tree, grass, terrain and avatar casters are untouched.
//
// FSPerfGPUDrivenDrawsVerify: after each multi-draw the same batches are drawn the stock way with depth
// writes off, a small polygon offset and a GL_SAMPLES_PASSED query; any sample that passes GL_LESS is a
// spot where the stock path would have put a nearer caster than the multi-draw did, which is logged. It
// also reads back a couple of arena entries per pass and compares them with their source buffers.

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivendraws.h"

#include "fsgddarena.h"
#include "fsgddcommon.h"
#include "fsgpudrivenlegacy.h"
#include "fsgpudrivenmaterials.h"
#include "fsgpudrivenalpha.h"
#include "fsgpudrivenpbr.h"
#include "fsgpudrivenshadowalpha.h"
#include "fsgpuocclusion.h"

#include "fsshadowsinglepass.h"
#include "lldrawpool.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <map>
#include <unordered_map>

LLGLSLShader gFSGDDShadowProgram;
LLGLSLShader gFSGDDShadowSPProgram;

namespace
{
    constexpr GLuint FS_GDD_MATRIX_BINDING = 7;     // shader storage binding of FSGDDMatrices
    constexpr U32 FS_GDD_VERIFY_SAMPLES = 64;       // samples a verify pass may let through before it warns

    const LLStaticHashedString sClip("fs_shadow_clip");
    const LLStaticHashedString sCascade("fs_shadow_cascade");

    // positions only, three quarters of FSPerfGPUDrivenDrawsArenaMB, the rest indices
    FSGDDArena sArena("FSPerfGPUDrivenDraws shadow arena", { LLVertexBuffer::TYPE_VERTEX }, "FSPerfGPUDrivenDrawsArenaMB", 256, 4);

    // per multi-draw data, rebuilt for each call
    FSGDDBuffer sIndirectBuffer;
    FSGDDBuffer sDrawBuffer;
    GLuint sQuery = 0;
    GLint sRecordAttrib[2] = { -1, -1 };   // location of fs_gdd_draw: plain, single pass
    S32 sInputsReady[2] = { 0, 0 };         // bind_inputs done: 0 not yet, 1 yes, -1 failed
    std::vector<FSGDDDrawCommand> sCommands;
    struct Record                           // fs_gdd_draw, one per instance of a command
    {
        U32 mMatrix;                        // index into fs_gdd_matrix
        U32 mCascade;                       // single pass: the cascade the instance draws into
    };
    std::vector<Record> sDraws;
    FSGDDMatrixTable sMatrices;

    // stats, logged every 10 seconds with FSPerfShadowBatchStats or FSPerfGPUDrivenDrawsVerify
    U64 sStatBatches = 0;
    U64 sStatCommands = 0;
    U64 sStatCalls = 0;
    U64 sStatLeft = 0;
}

namespace
{
    // the shadow pass's own GL objects; the PBR pass (fsGDDPBR*) releases its own
    void release_shadow_gl()
    {
        sArena.releaseGL();
        sIndirectBuffer.release();
        sDrawBuffer.release();
        sMatrices.release();
        fsGDDDeleteQuery(sQuery);
    }

    void unload_shadow_shaders()
    {
        gFSGDDShadowProgram.unload();
        gFSGDDShadowSPProgram.unload();
        sInputsReady[0] = 0;
        sInputsReady[1] = 0;
        release_shadow_gl();
    }
}

void fsGDDReleaseGL()
{
    fsGDDLegacyReleaseGL();
    fsGDDMaterialsReleaseGL();
    fsGDDAlphaReleaseGL();
    fsGDDPBRReleaseGL();
    fsGDDShadowAlphaReleaseGL();
    release_shadow_gl();
    FSGPUOcclusion::releaseGL();    // FSRenderGPUOcclusionCulling: its buffers, pyramid and programs
    fsGDDCommonReleaseGL();     // after the passes: the upload ring and the shared matrix tables
}

void fsGDDUnloadShaders()
{
    unload_shadow_shaders();
    fsGDDLegacyUnloadShaders(); // releases the legacy GL objects too
    fsGDDMaterialsUnloadShaders();  // and the material pass's
    fsGDDAlphaUnloadShaders();      // and the alpha pass's
    fsGDDPBRUnloadShaders();    // releases the PBR GL objects too
    fsGDDShadowAlphaUnloadShaders();
    FSGPUOcclusion::releaseGL();    // its compute programs are built again when next wanted
    fsGDDCommonReleaseGL();     // the upload ring and the shared matrix tables
}

namespace
{
    // the matrix block's binding, and where the driver put the per command record
    bool bind_inputs(LLGLSLShader& shader, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDMatrices");
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || record_attrib < 0)
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_MATRIX_BINDING);
        return true;
    }

    // createShader() may only submit the program (parallel shader compile); bind_inputs waits for its first use
    bool create_program(LLGLSLShader& shader, const char* name, S32 shader_level, S32 single_pass_ext)
    {
        shader.mName = name;
        shader.mShaderFiles.clear();
        shader.clearPermutations();
        shader.mShaderLevel = shader_level;
        shader.mShaderFiles.push_back(std::make_pair("deferred/fsGPUDrivenShadowV.glsl", GL_VERTEX_SHADER));
        shader.mShaderFiles.push_back(std::make_pair("deferred/shadowF.glsl", GL_FRAGMENT_SHADER));
        if (single_pass_ext > 0)
        {
            shader.addPermutation("FS_SHADOW_SINGLE_PASS", llformat("%d", single_pass_ext));
        }
        return shader.createShader();
    }
}

bool fsGDDLoadShaders(S32 shader_level, bool use_shadow)
{
    unload_shadow_shaders();

    // the PBR pass (FSPerfGPUDrivenPBR) has its own setting and doesn't need shadows; it unloads its own first
    fsGDDPBRLoadShaders();
    // so do the legacy passes (FSPerfGPUDrivenLegacy); after the PBR load, which tests bindless textures again
    fsGDDLegacyLoadShaders();
    // and the legacy material passes (FSPerfGPUDrivenMaterials)
    fsGDDMaterialsLoadShaders();
    // and the forward alpha pass (FSPerfGPUDrivenAlpha), also with bindless textures
    fsGDDAlphaLoadShaders();
    // and the alpha tested shadow casters (FSPerfGPUDrivenShadowAlpha), also with bindless textures
    fsGDDShadowAlphaLoadShaders(shader_level, use_shadow);

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenDraws", false);
    if (!enabled || !use_shadow)
    {
        return false;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenDraws: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object (in the vertex shader); shadow casters "
                                       << "are drawn as before" << LL_ENDL;
        return false;
    }

    if (!create_program(gFSGDDShadowProgram, "FS GPU-Driven Shadow Shader", shader_level, 0))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenDraws: the multi-draw shadow program failed to build, "
                                  << "shadow casters are drawn as before" << LL_ENDL;
        unload_shadow_shaders();    // not the PBR programs, they don't depend on this one
        return false;
    }

    // the single pass variant only when FSPerfShadowSinglePass has its own programs
    if (gFSShadowSinglePassShaders && gGLManager.mFSVertexViewportIndex > 0 &&
        !create_program(gFSGDDShadowSPProgram, "FS GPU-Driven Single Pass Shadow Shader", shader_level,
                        gGLManager.mFSVertexViewportIndex))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenDraws: the single pass multi-draw shadow program failed to build, "
                                  << "single pass shadow casters are drawn as before" << LL_ENDL;
        gFSGDDShadowSPProgram.unload();
    }
    return true;
}

bool fsGDDShadowEnabled(bool single_pass)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenDraws", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws)
    {
        return false;
    }
    if (sInputsReady[single_pass ? 1 : 0] < 0)
    {
        return false;
    }
    return single_pass ? gFSGDDShadowSPProgram.isComplete() : gFSGDDShadowProgram.isComplete();
}

namespace
{
    struct Item
    {
        LLDrawInfo* mParams;
        U32         mMask;      // single pass cascade set, 1 otherwise
    };

    struct Segment
    {
        U32 mMask;
        U32 mFirst;             // first command
        U32 mCount;
    };

    std::vector<Item> sItems;

    // Binds the program; the first time, also finds its inputs. False (for good) when they aren't there.
    bool bind_program(U32 which)
    {
        LLGLSLShader& program = which ? gFSGDDShadowSPProgram : gFSGDDShadowProgram;
        program.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &program)
        {
            return false;
        }
        if (!sInputsReady[which])
        {
            sInputsReady[which] = bind_inputs(program, sRecordAttrib[which]) ? 1 : -1;
            if (sInputsReady[which] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenDraws: " << program.mName << " lacks its matrix block or draw "
                                      << "record attribute, shadow casters are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[which] > 0;
    }
    std::vector<Item> sHandled;     // FSPerfGPUDrivenDrawsVerify
    std::vector<Segment> sSegments;

    // Turns the items (sorted by mask, model matrix, buffer, index offset, longer range first) into commands
    // and segments. Items that can't be drawn from the arena go to left, the rest (verify only) to sHandled.
    void build_commands(const std::vector<Item>& items, std::vector<Item>& left, bool single_pass, bool verify)
    {
        LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD);
        sCommands.clear();
        sDraws.clear();
        sSegments.clear();
        sHandled.clear();

        const U32 frame = LLFrameTimer::getFrameCount();
        sArena.beginFrame(frame);
        const glm::mat4 proj = gGL.getProjectionMatrix();
        const glm::mat4 view = get_current_modelview();
        sMatrices.begin(proj, view, false, false);

        const size_t count = items.size();
        size_t i = 0;
        while (i < count)
        {
            // The draw infos, buffers and arena entries are all over the heap: start loading them a few
            // draws ahead (each step needs the one before it in cache), so the lookups don't wait on memory
            if (i + 16 < count)
            {
                _mm_prefetch((const char*)items[i + 16].mParams, _MM_HINT_T0);
            }
            if (i + 8 < count)
            {
                if (const LLVertexBuffer* ahead = items[i + 8].mParams->mVertexBuffer.get())
                {
                    sArena.prefetchBuffer(ahead);
                }
            }
            if (i + 4 < count)
            {
                if (const LLVertexBuffer* ahead = items[i + 4].mParams->mVertexBuffer.get())
                {
                    sArena.prefetchEntry(ahead);
                }
            }
            const Item& first = items[i];
            LLVertexBuffer* vb = first.mParams->mVertexBuffer.get();
            const LLMatrix4* model = first.mParams->mModelMatrix;
            size_t group_end = i + 1;
            while (group_end < count && items[group_end].mMask == first.mMask &&
                   items[group_end].mParams->mModelMatrix == model && items[group_end].mParams->mVertexBuffer.get() == vb)
            {
                ++group_end;
            }

            const FSGDDArenaEntry* entry = sArena.find(vb, frame);
            bool in_range = entry != nullptr;
            for (size_t j = i; in_range && j < group_end; ++j)
            {   // always true for a valid batch; anything else is left to the stock path rather than read past the copy
                const LLDrawInfo* params = items[j].mParams;
                in_range = params->mOffset + params->mCount <= entry->mNumIndices && params->mEnd < entry->mNumVerts;
            }
            if (!in_range)
            {
                left.insert(left.end(), items.begin() + i, items.begin() + group_end);
                i = group_end;
                continue;
            }

            if (sSegments.empty() || sSegments.back().mMask != first.mMask)
            {
                sSegments.push_back({ first.mMask, (U32)sCommands.size(), 0 });
            }
            const U32 matrix = sMatrices.mvpIndex(model, proj, view);
            const U32 instances = single_pass ? fsGDDPopcount4(first.mMask) : 1;
            U32 cascades[4] = { 0, 0, 0, 0 };   // the cascade of each instance
            if (single_pass)
            {
                U32 n = 0;
                for (U32 c = 0; c < 4; ++c)
                {
                    if (first.mMask & (1u << c))
                    {
                        cascades[n++] = c;
                    }
                }
            }

            // a run of contiguous index ranges is one command; a range inside the one just added is dropped
            U32 offset = first.mParams->mOffset;
            U32 index_end = offset + first.mParams->mCount;
            for (size_t j = i + 1; j <= group_end; ++j)
            {
                const LLDrawInfo* params = j < group_end ? items[j].mParams : nullptr;
                if (params && params->mOffset + params->mCount <= index_end)
                {
                    continue;
                }
                if (params && params->mOffset == index_end)
                {
                    index_end += params->mCount;
                    continue;
                }
                // base instance: this command's first record, one record per instance
                sCommands.push_back({ index_end - offset, instances, entry->mIndex + offset, (GLint)entry->mVertex, (GLuint)sDraws.size() });
                for (U32 n = 0; n < instances; ++n)
                {
                    sDraws.push_back({ matrix, cascades[n] });
                }
                ++sSegments.back().mCount;
                if (params)
                {
                    offset = params->mOffset;
                    index_end = offset + params->mCount;
                }
            }
            if (verify)
            {
                sHandled.insert(sHandled.end(), items.begin() + i, items.begin() + group_end);
            }
            i = group_end;
        }
    }

    // Issues the built commands, one multi-draw per segment. The program is bound by the caller.
    void draw_commands(bool single_pass)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd multi-draw");
        LL_PROFILE_GPU_ZONE("gdd multi-draw");

        sIndirectBuffer.upload(GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(FSGDDDrawCommand));
        sMatrices.upload();
        sMatrices.bindBase(FS_GDD_MATRIX_BINDING);

        // the per instance records, an instanced attribute (divisor 1) stepped from the commands' base
        // instances; it goes into the global vertex array object, never a buffer's own
        LLVertexBuffer::fsUseDefaultVAO();
        const GLuint record = (GLuint)sRecordAttrib[single_pass ? 1 : 0];
        sDrawBuffer.upload(GL_ARRAY_BUFFER, sDraws.data(), sDraws.size() * sizeof(Record));
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 2, GL_UNSIGNED_INT, sizeof(Record), sDrawBuffer.at(0));
        glVertexAttribDivisor(record, 1);
        sArena.bindForDraw(LLVertexBuffer::MAP_VERTEX);

        for (const Segment& segment : sSegments)
        {
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
                                        sIndirectBuffer.at((size_t)segment.mFirst * sizeof(FSGDDDrawCommand)), (GLsizei)segment.mCount, 0);
        }

        // the program's attribute mask doesn't have this location, so LLVertexBuffer expects it off and undivided
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MATRIX_BINDING, 0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
        STOP_GLERROR;
    }

    // FSPerfGPUDrivenDrawsVerify: the handled batches again, the stock way, with depth writes off and a GL_LESS
    // test against what the multi-draw left. The stock program for this pass must be bound.
    void verify_against_stock(bool single_pass)
    {
        if (sHandled.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd verify");

        // arena copies against their sources, a couple per pass
        fsGDDVerifyArenaCopies(sArena, sHandled, [](const Item& item) { return item.mParams->mVertexBuffer.get(); },
                               "FSPerfGPUDrivenDrawsVerify");

        const GLuint samples = fsGDDNearerSamples(sQuery, true, [single_pass]()
        {
            U32 mask = 0;
            for (const Item& item : sHandled)
            {
                if (single_pass && item.mMask != mask)
                {
                    mask = item.mMask;
                    F32 cascades[4];
                    const U32 n = fsGDDCascadeList(mask, cascades);
                    LLGLSLShader::sCurBoundShaderPtr->uniform4f(sCascade, cascades[0], cascades[1], cascades[2], cascades[3]);
                    LLVertexBuffer::sFSInstanceCount = n;
                }
                LLDrawInfo& params = *item.mParams;
                LLRenderPass::applyModelMatrix(params.mModelMatrix);
                params.mVertexBuffer->setBuffer();
                params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
            }
            LLVertexBuffer::sFSInstanceCount = 0;
        });
        gGL.loadMatrix(gGLModelView);
        gGLLastMatrix = NULL;

        if (samples > FS_GDD_VERIFY_SAMPLES)
        {
            static LLFrameTimer warn_timer;
            static U32 mismatches = 0;
            ++mismatches;
            if (fsGDDWarnDue(warn_timer))
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenDrawsVerify: " << samples << " samples of " << sHandled.size()
                                      << " stock shadow casters are nearer than the multi-draw depth"
                                      << (single_pass ? " (single pass)" : "") << "; " << mismatches
                                      << " mismatching passes so far" << LL_ENDL;
            }
        }
    }

    void count_stats(U32 batches, U32 calls, U32 left)
    {
        static LLCachedControl<bool> batch_stats(gSavedSettings, "FSPerfShadowBatchStats", false);
        static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);
        if (!batch_stats && !verify)
        {
            return;
        }
        sStatBatches += batches;
        sStatCommands += sCommands.size();
        sStatCalls += calls;
        sStatLeft += left;
        static LLFrameTimer timer;
        if (timer.getElapsedTimeF32() >= 10.f)
        {
            LL_INFOS("FSShadowBatches") << "GPU-driven shadow casters: " << sStatBatches << " batches, "
                                        << sStatCommands << " draw commands in " << sStatCalls << " multi-draw calls, "
                                        << sStatLeft << " batches left to the stock path; arena " << sArena.vertexMB()
                                        << " MB positions, " << sArena.indexMB() << " MB indices (" << sArena.allocatedMB() << " MB allocated), "
                                        << sArena.uploads() << " buffer copies, " << sArena.fallbacks()
                                        << " buffers not in the arena" << LL_ENDL;
            sStatBatches = 0;
            sStatCommands = 0;
            sStatCalls = 0;
            sStatLeft = 0;
            sArena.clearCounts();
            timer.reset();
        }
    }
}

U32 fsGDDDrawShadowOpaque(std::vector<LLDrawInfo*>& batches)
{
    if (batches.empty() || !fsGDDShadowEnabled(false))
    {
        return 0;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);

    LLGLSLShader* stock = LLGLSLShader::sCurBoundShaderPtr;
    if (!stock)
    {
        return 0;
    }

    sItems.clear();
    for (LLDrawInfo* params : batches)
    {
        sItems.push_back({ params, 1 });
    }
    std::sort(sItems.begin(), sItems.end(), [](const Item& a, const Item& b)
    {
        const LLDrawInfo* lhs = a.mParams;
        const LLDrawInfo* rhs = b.mParams;
        if (lhs->mModelMatrix != rhs->mModelMatrix)
        {
            return lhs->mModelMatrix < rhs->mModelMatrix;
        }
        if (lhs->mVertexBuffer.get() != rhs->mVertexBuffer.get())
        {
            return lhs->mVertexBuffer.get() < rhs->mVertexBuffer.get();
        }
        if (lhs->mOffset != rhs->mOffset)
        {
            return lhs->mOffset < rhs->mOffset;
        }
        return lhs->mCount > rhs->mCount;
    });

    static std::vector<Item> left;
    left.clear();
    build_commands(sItems, left, false, verify);

    const U32 total = (U32)batches.size();
    U32 calls = 0;
    if (!sCommands.empty() && !bind_program(0))
    {   // the arena copies stay for the next try; this pass goes the stock way
        stock->bind();
        return 0;
    }
    if (!sCommands.empty())
    {
        draw_commands(false);
        calls = (U32)sSegments.size();
        stock->bind();
        if (verify)
        {
            verify_against_stock(false);
        }
    }

    batches.clear();
    for (const Item& item : left)
    {
        batches.push_back(item.mParams);
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
    count_stats(total, calls, (U32)batches.size());
    return calls;
}

U32 fsGDDDrawShadowSinglePass(std::vector<LLPipeline::FSShadowSPEntry>& list, const glm::mat4* clip)
{
    if (list.empty() || !fsGDDShadowEnabled(true))
    {
        return 0;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);

    // already sorted by mask, model matrix, buffer and offset (fsShadowSinglePassFinish)
    sItems.clear();
    for (const LLPipeline::FSShadowSPEntry& entry : list)
    {
        sItems.push_back({ entry.mParams, entry.mMask });
    }

    static std::vector<Item> left;
    left.clear();
    build_commands(sItems, left, true, verify);

    const U32 total = (U32)list.size();
    U32 calls = 0;
    if (!sCommands.empty() && !bind_program(1))
    {   // the list is untouched, the caller draws it
        return 0;
    }
    if (!sCommands.empty())
    {
        gFSGDDShadowSPProgram.uniformMatrix4fv(sClip, 4, GL_FALSE, glm::value_ptr(clip[0]));
        draw_commands(true);
        calls = (U32)sSegments.size();
        if (verify && gFSShadowSPProgram.isComplete())
        {
            gFSShadowSPProgram.bind();
            gFSShadowSPProgram.uniformMatrix4fv(sClip, 4, GL_FALSE, glm::value_ptr(clip[0]));
            verify_against_stock(true);
        }
    }

    list.clear();
    for (const Item& item : left)
    {
        list.push_back({ item.mParams, item.mMask });
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
    count_stats(total, calls, (U32)list.size());
    return calls;
}
