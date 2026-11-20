/**
 * @file fsskinonce.cpp
 * @brief Rigged meshes skinned once a frame in a batched compute pre-pass (FSRenderSkinOnce)
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

// See fsskinonce.h for what this does. How:
//
// The draw hook (LLVertexBuffer::drawRange, object skinning programs only) keeps one entry per rigged vertex
// buffer and palette (avatar and skin) it sees: a buffer can hold the faces of several avatars (batches only
// break on texture, material and size), each drawn with its own palette. An entry holds the buffer (so its
// address stays its own), the avatar (by ID, looked up again before use) and skin of the palette the draw bound
// (noteSkin), and how many vertices the draws after the last pre-pass skinned, over which range of the buffer.
// The pre-pass (beginFrame, once a frame, before the sun shadows) picks the entries whose range was skinned at
// least FSRenderSkinOnceMinPasses times its size last frame, builds their palettes for this frame
// (updateSkinInfoMatrixPalette: what the stock loops would build later this frame anyway), writes the palettes
// into one storage buffer, gives each entry a range of the output buffer and runs one dispatch per entry reading
// the buffer's own GL buffer (no copy), then one barrier for vertex attribute reads. The output buffer is one of
// two taking turns by frame, so a frame never writes what the one before reads.
//
// The hook then serves a draw from the output when its entry was skinned this frame over the draw's range, the
// buffer's content serial is the one skinned, and the palette the draw bound is the build skinned; else it puts
// the buffer's own attributes back and leaves fs_skin_once at 0. Entries not drawn for a few frames are dropped.
// Frames are display() calls (gFrameCount, as the palette cache counts them): a tiled snapshot draws several.
// With mirrors on, the pre-pass runs in the first hero probe face, before this frame's object updates and
// rebuilds: a buffer rebuilt after it has a new content serial and its draws fall back, as they must.
//
// Main thread only; with FSRenderThread on, every GL call here is recorded in stream order like any other, and
// nothing reads back (the program is built once in a direct scope, like the other compute programs).

#include "llviewerprecompiledheaders.h"

#include "fsskinonce.h"

#include "fsglstream.h"

#include "lldir.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llskinningutil.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"

#include <sstream>
#include <unordered_map>
#include <vector>

extern U32 gFrameCount;

namespace
{
    constexpr U32 FS_NONE = 0xFFFFFFFFu;
    constexpr U32 OUT_REGIONS = 3;                      // positions, normals, tangents
    constexpr U32 VEC4_BYTES = 16;
    constexpr U32 OUT_VERTEX_BYTES = OUT_REGIONS * VEC4_BYTES;
    constexpr U32 MIN_OUT_VERTICES = 64 * 1024;
    constexpr U32 KEEP_FRAMES = 3;                      // entries not drawn for longer are dropped
    constexpr U32 GROUP_SIZE = 64;                      // local_size_x of fsSkinOnceC.glsl

    // A buffer and the palette (avatar, skin) a draw of it bound. The pointers are only compared: the entry holds
    // the buffer and the skin, and checks the avatar by its ID.
    struct Key
    {
        const LLVertexBuffer* mVB;
        const LLVOAvatar* mAvatar;
        const LLMeshSkinInfo* mSkin;
        bool operator==(const Key& other) const
        {
            return mVB == other.mVB && mAvatar == other.mAvatar && mSkin == other.mSkin;
        }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            size_t h = std::hash<const void*>()(k.mVB);
            h ^= std::hash<const void*>()(k.mAvatar) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<const void*>()(k.mSkin) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct Entry
    {
        LLPointer<LLVertexBuffer> mVB;
        LLUUID mAvatarID;
        const LLVOAvatar* mAvatar = nullptr;            // compared with the lookup by ID, never used unchecked
        LLConstPointer<LLMeshSkinInfo> mSkin;
        U32 mLastSeen = 0;                              // frame a draw last skinned it
        U64 mDrawn = 0;                                 // vertices the draws since the last pre-pass skinned
        U32 mLo = U32_MAX;                              // ...over this range of the buffer's vertices
        U32 mHi = 0;
        // the skinned copy
        U32 mFrame = U32_MAX;                           // frame it was made in
        U32 mFirst = 0;                                 // where the buffer's vertex 0 would be in the output's regions
        U32 mSkinLo = 0;                                // the buffer's vertices skinned
        U32 mSkinHi = 0;
        U32 mBuffer = 0;                                // the output buffer
        U32 mCapacity = 0;                              // its vertices per region
        U32 mPaletteSerial = 0;                         // MatrixPaletteCache::mFSBuildSerial skinned with
        U32 mContentSerial = 0;                         // LLVertexBuffer::getFSContentSerial skinned
    };

    struct Output
    {
        GLuint mName = 0;
        U32 mCapacity = 0;                              // vertices per region
        U32 mSmallFrames = 0;                           // frames in a row it was over four times too big
    };

    struct Job
    {
        Entry* mEntry;
        U32 mFirst;                                     // output vertex of the buffer's vertex mLo
        U32 mPalette;                                   // header's vec4 index in the palette buffer
        U32 mPaletteSerial;
        U32 mLo;                                        // the buffer's vertices to skin
        U32 mCount;
    };

    // The palette the next draws use (noteSkin), this frame only. The avatar's ID is taken when the palette is
    // uploaded (the avatar is alive then) and the skin is held: a draw made before the next frame's pre-pass
    // (dynamic textures, the model preview) never reads an avatar that went away in between.
    struct Note
    {
        const LLVOAvatar* mAvatar = nullptr;            // only compared
        LLUUID mAvatarID;
        LLConstPointer<LLMeshSkinInfo> mSkin;
        U32 mSerial = 0;
        U32 mFrame = U32_MAX;
    };

    // Counts since the last stats line
    struct Stats
    {
        U64 mFrames = 0;
        U64 mJobs = 0;              // buffers skinned in the pre-pass
        U64 mSkinned = 0;           // vertices skinned in the pre-pass
        U64 mServedDraws = 0;       // draws that read the skinned copy
        U64 mServedVerts = 0;       // vertices those draws covered (instances counted)
        U64 mFallbackDraws = 0;     // skinning draws that skinned in the shader
        U64 mFallbackVerts = 0;
        U64 mNew = 0;               // of those: the buffer not seen before
        U64 mBelow = 0;             // not skinned ahead: drawn too few times last frame
        U64 mStale = 0;             // skinned ahead, but the content or palette changed since
        U64 mBudget = 0;            // left out: the output budget used up
        U64 mUnusable = 0;          // left out: no avatar, palette or GL buffer, or mapped
        U64 mNoUniform = 0;         // the program has no fs_skin_once (not built with FS_SKIN_ONCE)
        U64 mRebinds = 0;           // attribute pointer changes for served draws
        U32 mOutVertices = 0;       // output capacity at the last pre-pass (both buffers)
    };

    bool sBuilt = false;            // shaders built with FS_SKIN_ONCE and the GL can run it
    bool sProgramTried = false;
    GLuint sProgram = 0;
    GLint sSrcRegions = -1;
    GLint sJob = -1;
    U64 sMaxBlockBytes = 0;         // GL_MAX_SHADER_STORAGE_BLOCK_SIZE
    Output sOut[2];
    U32 sPing = 0;
    GLuint sPalettes = 0;
    U32 sFrame = U32_MAX;           // frame of the last pre-pass
    Note sNote;
    std::unordered_map<Key, Entry, KeyHash> sEntries;
    std::vector<Job> sJobs;
    std::vector<F32> sPaletteData;
    std::unordered_map<U32, U32> sPaletteBases;  // build serial -> header vec4 index, this pre-pass
    Stats sStats;
    F64 sStatsSince = -1.0;

    bool gl_supported()
    {
        return !gGLManager.mIsApple && gGLManager.mGLVersion >= 4.29f && glDispatchCompute && glMemoryBarrier &&
               glBindBufferBase && glClearBufferData && glUniform4ui && glGetInteger64v;
    }

    std::string read_shader(const std::string& file)
    {
        const std::string& delim = gDirUtilp->getDirDelimiter();
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders",
                                                                "class1" + delim + "avatar" + delim + file);
        llifstream in(path.c_str());
        if (!in.is_open())
        {
            LL_WARNS("ShaderLoading") << "FSRenderSkinOnce: can't open " << path << LL_ENDL;
            return std::string();
        }
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // Once per GL context (release starts over)
    bool ensure_program()
    {
        if (sProgramTried)
        {
            return sProgram != 0;
        }
        sProgramTried = true;
        const std::string body = read_shader("fsSkinOnceC.glsl");
        if (body.empty())
        {
            return false;
        }
        // one burst of synchronous calls instead of a wait each
        FSGLStream::DirectScope direct;

        auto global = [](const char* name) -> std::string
        {
            auto it = LLGLSLShader::sGlobalDefines.find(name);
            return it != LLGLSLShader::sGlobalDefines.end() ? it->second : std::string();
        };
        std::string source = "#version 430 core\n";
        source += "#define MAX_JOINTS_PER_MESH_OBJECT " + std::to_string(LLSkinningUtil::getMaxJointCount()) + "\n";
        source += "#define SKIN_PRECISE " + global("SKIN_PRECISE") + "\n";
        source += body;
        const GLchar* text = source.c_str();

        GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
        if (!shader)
        {
            return false;
        }
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE)
        {
            GLchar log[2048] = {};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            LL_WARNS("ShaderLoading") << "FSRenderSkinOnce: the skinning program failed to compile: " << log << LL_ENDL;
            glDeleteShader(shader);
            return false;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, shader);
        glLinkProgram(program);
        glDeleteShader(shader);     // goes with the program
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE)
        {
            GLchar log[2048] = {};
            glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
            LL_WARNS("ShaderLoading") << "FSRenderSkinOnce: the skinning program failed to link: " << log << LL_ENDL;
            glDeleteProgram(program);
            return false;
        }
        sSrcRegions = glGetUniformLocation(program, "fs_src_regions");
        sJob = glGetUniformLocation(program, "fs_job");
        GLint64 max_block = 0;
        glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_block);
        sMaxBlockBytes = (U64)llmax(max_block, (GLint64)0);
        if (sSrcRegions < 0 || sJob < 0 || sMaxBlockBytes < (U64)MIN_OUT_VERTICES * OUT_VERTEX_BYTES)
        {
            LL_WARNS("ShaderLoading") << "FSRenderSkinOnce: the skinning program lacks its inputs; rigged meshes are "
                                      << "skinned in every pass as before" << LL_ENDL;
            glDeleteProgram(program);
            return false;
        }
        sProgram = program;
        LL_INFOS("ShaderLoading") << "FSRenderSkinOnce: skinning program ready, storage blocks up to "
                                  << (sMaxBlockBytes >> 20) << " MB" << LL_ENDL;
        return true;
    }

    void free_output(Output& out)
    {
        if (out.mName)
        {
            LLVertexBuffer::fsAddExternalBytes(-(S64)out.mCapacity * OUT_VERTEX_BYTES);
            glDeleteBuffers(1, &out.mName);
        }
        out = Output();
    }

    // Storage for at least `need` vertices a region, at most `limit`. New storage is cleared (no undefined
    // floats ever reach a vertex shader, whatever the driver hands out).
    bool ensure_output(Output& out, U32 need, U32 limit)
    {
        U32 want = out.mCapacity;
        if (out.mCapacity < need)
        {
            want = llmin(llmax(need + need / 4, MIN_OUT_VERTICES), limit);
            out.mSmallFrames = 0;
        }
        else if (out.mCapacity > MIN_OUT_VERTICES && need < out.mCapacity / 4)
        {   // much too big for a while (a crowd left): give most of it back
            if (++out.mSmallFrames > 600)
            {
                want = llmax(need + need / 4, MIN_OUT_VERTICES);
                out.mSmallFrames = 0;
            }
        }
        else
        {
            out.mSmallFrames = 0;
        }
        if (want == out.mCapacity && out.mName)
        {
            return true;
        }
        if (want < need)
        {
            return false;
        }
        if (!out.mName)
        {
            glGenBuffers(1, &out.mName);
            if (!out.mName)
            {
                return false;
            }
        }
        else
        {
            LLVertexBuffer::fsAddExternalBytes(-(S64)out.mCapacity * OUT_VERTEX_BYTES);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, out.mName);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)want * OUT_VERTEX_BYTES, nullptr, GL_DYNAMIC_COPY);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, nullptr);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        out.mCapacity = want;
        LLVertexBuffer::fsAddExternalBytes((S64)want * OUT_VERTEX_BYTES);
        return true;
    }

    // The palette's header index in this pre-pass's palette data, added once per build
    U32 place_palette(const LLVOAvatar::MatrixPaletteCache& mpc, U32 count)
    {
        auto it = sPaletteBases.find(mpc.mFSBuildSerial);
        if (it != sPaletteBases.end())
        {
            return it->second;
        }
        const U32 base = (U32)(sPaletteData.size() / 4);
        // the header and the std140 mat3x4 columns the FS_GDD_RIGGED skinning reads (fsskinpalettebuffer.cpp)
        sPaletteData.push_back(mpc.mSkinOrigin.mV[VX]);
        sPaletteData.push_back(mpc.mSkinOrigin.mV[VY]);
        sPaletteData.push_back(mpc.mSkinOrigin.mV[VZ]);
        sPaletteData.push_back((F32)count);
        sPaletteData.insert(sPaletteData.end(), mpc.mGLMp.begin(), mpc.mGLMp.begin() + (size_t)count * 12);
        sPaletteBases[mpc.mFSBuildSerial] = base;
        return base;
    }

    // The avatar a kept entry names, if it is still there and alive
    LLVOAvatar* entry_avatar(const Entry& e)
    {
        if (e.mAvatarID.isNull())
        {
            return nullptr;
        }
        LLViewerObject* object = gObjectList.findObject(e.mAvatarID);
        if (!object || object != (const LLViewerObject*)e.mAvatar || object->isDead() || !object->isAvatar())
        {
            return nullptr;
        }
        return static_cast<LLVOAvatar*>(object);
    }

    void log_stats()
    {
        static LLCachedControl<bool> stats(gSavedSettings, "FSPerfSkinOnceStats", false);
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (sStatsSince < 0.0)
        {
            sStatsSince = now;
        }
        if (now - sStatsSince < 10.0)
        {
            return;
        }
        if (stats && sStats.mFrames)
        {
            const F64 f = (F64)sStats.mFrames;
            const F64 reuse = sStats.mSkinned ? (F64)sStats.mServedVerts / (F64)sStats.mSkinned : 0.0;
            LL_INFOS("FSSkinOnce") << "FSRenderSkinOnce, per frame over " << sStats.mFrames << " frames: skinned once "
                                   << (U64)(sStats.mJobs / f) << " buffers, " << (U64)(sStats.mSkinned / f)
                                   << " vertices; reused by " << (U64)(sStats.mServedDraws / f) << " draws, "
                                   << (U64)(sStats.mServedVerts / f) << " vertices (" << llformat("%.2f", reuse)
                                   << " passes per vertex skinned once); skinned in the shader " << (U64)(sStats.mFallbackDraws / f)
                                   << " draws, " << (U64)(sStats.mFallbackVerts / f) << " vertices (new buffer "
                                   << (U64)(sStats.mNew / f) << ", drawn too few times " << (U64)(sStats.mBelow / f)
                                   << ", changed since " << (U64)(sStats.mStale / f) << ", over budget "
                                   << (U64)(sStats.mBudget / f) << ", unusable " << (U64)(sStats.mUnusable / f)
                                   << ", program without it " << (U64)(sStats.mNoUniform / f) << "); attribute rebinds "
                                   << (U64)(sStats.mRebinds / f) << "; " << sEntries.size() << " buffers tracked; output "
                                   << (((U64)sStats.mOutVertices * OUT_VERTEX_BYTES) >> 20) << " MB" << LL_ENDL;
        }
        sStats = Stats();
        sStatsSince = now;
    }

    // fs_skin_once's location in the bound program, looked up once per program (not twice a draw); forgotten when
    // the shaders are released
    const LLGLSLShader* sLocShader = nullptr;
    GLuint sLocProgram = 0;
    GLint sLocation = -1;
    GLint skin_once_location(LLGLSLShader* shader)
    {
        static LLStaticHashedString s_skin_once("fs_skin_once");
        if (shader != sLocShader || shader->mProgramObject != sLocProgram)
        {
            sLocShader = shader;
            sLocProgram = shader->mProgramObject;
            sLocation = shader->getUniformLocation(s_skin_once);
        }
        return sLocation;
    }

    // As LLGLSLShader::uniform1i does, with the location already known
    void set_skin_once(LLGLSLShader* shader, GLint location, GLint value)
    {
        if (location < 0)
        {
            return;
        }
        const LLVector4 vec((F32)value, 0.f, 0.f, 0.f);
        auto it = shader->mValue.find(location);
        if (it == shader->mValue.end() || it->second.mV[VX] != vec.mV[VX])
        {
            glUniform1i(location, value);
            shader->mValue[location] = vec;
        }
    }

    bool note_valid(U32 frame)
    {
        return sNote.mFrame == frame && sNote.mAvatar && sNote.mSkin.notNull();
    }

    // The entry's copy can serve a draw of [start, end] this frame
    bool entry_fresh(const Entry& e, const LLVertexBuffer* vb, U32 frame, U32 start, U32 end)
    {
        return e.mFrame == frame && e.mBuffer && e.mPaletteSerial != 0 && e.mPaletteSerial == sNote.mSerial &&
               e.mContentSerial == vb->getFSContentSerial() && !vb->isFSMapped() && start >= e.mSkinLo && end <= e.mSkinHi;
    }

    // LLVertexBuffer::sFSSkinOnceServes: setBuffer() asks whether the coming draws of vb will read its skinned
    // copy, so it sets the buffer up in the global vertex array object those draws use instead of in the
    // buffer's own one, which they would leave at once (and set every pointer up a second time)
    bool serves(const LLVertexBuffer* vb)
    {
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        const U32 frame = gFrameCount;
        if (!vb || !shader || !shader->mFeatures.hasObjectSkinning || !note_valid(frame) || sFrame != frame)
        {
            return false;
        }
        auto it = sEntries.find(Key{ vb, sNote.mAvatar, sNote.mSkin.get() });
        return it != sEntries.end() && it->second.mAvatarID == sNote.mAvatarID &&
               entry_fresh(it->second, vb, frame, it->second.mSkinLo, it->second.mSkinHi) &&
               skin_once_location(shader) >= 0;
    }

    // LLVertexBuffer::sFSSkinOnceHook: before a draw with an object skinning program
    void draw_hook(const LLVertexBuffer* vb, U32 start, U32 end)
    {
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!vb || !shader)
        {
            return;
        }
        const U32 frame = gFrameCount;
        const U64 verts = (U64)(end >= start ? end - start + 1 : 0) * llmax(LLVertexBuffer::sFSInstanceCount, 1u);
        const GLint location = skin_once_location(shader);
        // only draws after this frame's pre-pass can read a copy: only they count towards making one
        const bool after_prepass = sFrame == frame;

        auto fallback = [&]()
        {
            vb->fsEndPreskinned();
            set_skin_once(shader, location, 0);
            ++sStats.mFallbackDraws;
            sStats.mFallbackVerts += verts;
        };

        if (!note_valid(frame))
        {   // no palette noted this frame: nothing to key the copy by
            fallback();
            ++sStats.mUnusable;
            return;
        }

        const Key key{ vb, sNote.mAvatar, sNote.mSkin.get() };
        auto it = sEntries.find(key);
        if (it == sEntries.end())
        {
            fallback();
            ++sStats.mNew;
            Entry& e = sEntries[key];
            e.mVB = const_cast<LLVertexBuffer*>(vb);
            e.mAvatarID = sNote.mAvatarID;
            e.mAvatar = sNote.mAvatar;
            e.mSkin = sNote.mSkin;
            e.mLastSeen = frame;
            if (after_prepass)
            {
                e.mDrawn = verts;
                e.mLo = start;
                e.mHi = end;
            }
            return;
        }

        Entry& e = it->second;
        if (e.mAvatarID != sNote.mAvatarID)
        {   // another avatar at a freed one's address: its copy is not this one's
            e.mAvatarID = sNote.mAvatarID;
            e.mFrame = U32_MAX;
            e.mDrawn = 0;
            e.mLo = U32_MAX;
            e.mHi = 0;
        }
        e.mLastSeen = frame;
        if (after_prepass)
        {
            e.mDrawn += verts;
            e.mLo = llmin(e.mLo, start);
            e.mHi = llmax(e.mHi, end);
        }

        const bool fresh = entry_fresh(e, vb, frame, start, end);
        if (fresh && location >= 0)
        {
            if (vb->fsBindPreskinned(e.mBuffer, e.mFirst, e.mCapacity))
            {
                ++sStats.mRebinds;
            }
            set_skin_once(shader, location, 1);
            ++sStats.mServedDraws;
            sStats.mServedVerts += verts;
            return;
        }

        fallback();
        if (fresh)
        {
            ++sStats.mNoUniform;
        }
        else if (e.mFrame == frame)
        {
            ++sStats.mStale;
        }
        else
        {
            ++sStats.mBelow;
        }
    }

    void install_hook(bool install)
    {
        LLVertexBuffer::sFSSkinOnceHook = install ? &draw_hook : nullptr;
        LLVertexBuffer::sFSSkinOnceServes = install ? &serves : nullptr;
    }
}

bool FSSkinOnce::wanted()
{
    static LLCachedControl<bool> setting(gSavedSettings, "FSRenderSkinOnce", false);
    return setting;
}

void FSSkinOnce::configureShaders(std::map<std::string, std::string>& global_defines)
{
    release();
    sBuilt = wanted() && gl_supported();
    if (sBuilt)
    {
        global_defines["FS_SKIN_ONCE"] = "1";
        install_hook(true);
    }
    else if (wanted())
    {
        LL_INFOS("ShaderLoading") << "FSRenderSkinOnce: needs OpenGL 4.3 compute shaders; rigged meshes are skinned "
                                  << "in every pass as before" << LL_ENDL;
    }
}

void FSSkinOnce::release()
{
    install_hook(false);
    LLVertexBuffer::invalidateAttributeSetup();
    if (sProgram)
    {
        glDeleteProgram(sProgram);
    }
    sProgram = 0;
    sProgramTried = false;
    sSrcRegions = -1;
    sJob = -1;
    for (Output& out : sOut)
    {
        free_output(out);
    }
    if (sPalettes)
    {
        glDeleteBuffers(1, &sPalettes);
    }
    sPalettes = 0;
    sEntries.clear();
    sJobs.clear();
    sPaletteData.clear();
    sPaletteBases.clear();
    sNote = Note();
    sFrame = U32_MAX;
    sBuilt = false;
    sLocShader = nullptr;
    sLocProgram = 0;
    sLocation = -1;
}

void FSSkinOnce::noteSkinImpl(const LLVOAvatar* avatar, const LLMeshSkinInfo* skin, U32 palette_serial)
{
    const U32 frame = gFrameCount;
    if (avatar != sNote.mAvatar || skin != sNote.mSkin.get() || frame != sNote.mFrame)
    {
        sNote.mAvatar = avatar;
        sNote.mAvatarID = avatar ? avatar->getID() : LLUUID::null;
        sNote.mSkin = skin;
        sNote.mFrame = frame;
    }
    sNote.mSerial = palette_serial;
}

void FSSkinOnce::beginFrame()
{
    if (!sBuilt || !LLVertexBuffer::sFSSkinOnceHook)
    {
        return;
    }
    const U32 frame = gFrameCount;
    if (frame == sFrame)
    {
        return;
    }
    sFrame = frame;
    // a palette noted last frame is no promise for this one (and holds a skin that may be going away)
    sNote = Note();
    log_stats();
    if (!wanted())
    {   // switched off, the shaders not reloaded yet: no copies, every draw skins in the shader
        return;
    }
    if (!ensure_program())
    {
        LL_WARNS_ONCE("Rendering") << "FSRenderSkinOnce: no skinning program; rigged meshes are skinned in every pass "
                                   << "as before" << LL_ENDL;
        install_hook(false);
        sEntries.clear();
        return;
    }
    LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("skin once");
    ++sStats.mFrames;

    static LLCachedControl<F32> min_passes_setting(gSavedSettings, "FSRenderSkinOnceMinPasses", 3.f);
    static LLCachedControl<S32> budget_mb(gSavedSettings, "FSRenderSkinOnceBudgetMB", 192);
    const F64 min_passes = llclamp((F64)min_passes_setting, 1.0, 64.0);
    const U64 budget_bytes = llmin((U64)llclamp((S32)budget_mb, 16, 4096) << 20, sMaxBlockBytes);
    const U32 limit = (U32)llmin(budget_bytes / OUT_VERTEX_BYTES, (U64)U32_MAX / OUT_REGIONS);

    sJobs.clear();
    sPaletteData.clear();
    sPaletteBases.clear();
    U64 total = 0;
    for (auto it = sEntries.begin(); it != sEntries.end();)
    {
        Entry& e = it->second;
        // what the draws since the last pre-pass (last frame's, after its pre-pass) skinned
        const U64 drawn = e.mDrawn;
        const U32 drawn_lo = e.mLo;
        const U32 drawn_hi = e.mHi;
        e.mDrawn = 0;
        e.mLo = U32_MAX;
        e.mHi = 0;
        e.mFrame = U32_MAX;
        if (frame - e.mLastSeen > KEEP_FRAMES || e.mVB.isNull())
        {   // left view, a LOD changed, gone: drop it (and the buffer, the skin it holds)
            it = sEntries.erase(it);
            continue;
        }
        ++it;

        // Only the range last frame's draws of this palette covered: the buffer's other faces may be another
        // avatar's, skinned with its own palette by an entry of their own
        LLVertexBuffer* vb = e.mVB.get();
        const U32 buffer_verts = vb->getNumVerts();
        if (!drawn || !buffer_verts || drawn_lo > drawn_hi || drawn_lo >= buffer_verts)
        {
            continue;
        }
        const U32 lo = drawn_lo;
        const U32 hi = llmin(drawn_hi, buffer_verts - 1);
        const U32 verts = hi - lo + 1;
        if ((F64)drawn < min_passes * (F64)verts)
        {
            continue;
        }
        LLVOAvatar* avatar = entry_avatar(e);
        if (!avatar || e.mSkin.isNull() || !vb->getFSGLBuffer() || vb->isFSMapped() ||
            !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX) || !vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4))
        {
            ++sStats.mUnusable;
            continue;
        }
        // The draws address the copy by the buffer's own vertex numbers, so vertex lo goes at least lo vertices
        // into the output (the attribute pointers can't start before it)
        const U64 first = llmax(total, (U64)lo);
        if (first + verts > limit)
        {
            ++sStats.mBudget;
            continue;
        }
        // what the stock loops build later this frame anyway (built once a frame per avatar and skin)
        const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(e.mSkin.get());
        const U32 count = (U32)llmin(mpc.mMatrixPalette.size(), (size_t)LLSkinningUtil::getMaxJointCount());
        if (!count || !mpc.mFSBuildSerial || mpc.mGLMp.size() < (size_t)count * 12)
        {
            ++sStats.mUnusable;
            continue;
        }
        sJobs.push_back({ &e, (U32)first, place_palette(mpc, count), mpc.mFSBuildSerial, lo, verts });
        total = first + verts;
    }
    if (sJobs.empty())
    {
        return;
    }

    Output& out = sOut[sPing];
    if (!ensure_output(out, (U32)total, limit))
    {
        return;
    }
    sStats.mOutVertices = sOut[0].mCapacity + sOut[1].mCapacity;

    LL_PROFILE_GPU_ZONE("skin once");
    if (!sPalettes)
    {
        glGenBuffers(1, &sPalettes);
        if (!sPalettes)
        {
            return;
        }
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sPalettes);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sPaletteData.size() * sizeof(F32)), sPaletteData.data(),
                 GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glUseProgram(sProgram);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, out.mName);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sPalettes);
    for (const Job& job : sJobs)
    {
        Entry& e = *job.mEntry;
        LLVertexBuffer* vb = e.mVB.get();
        const U32 verts = job.mCount;
        const U32 lo = job.mLo;
        // each region read from vertex lo on (16 bytes a vertex in every one of them)
        auto region = [vb, lo](LLVertexBuffer::AttributeType type)
        {
            return vb->hasDataType(type) ? vb->getOffset(type) / VEC4_BYTES + lo : FS_NONE;
        };
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, vb->getFSGLBuffer());
        glUniform4ui(sSrcRegions, region(LLVertexBuffer::TYPE_VERTEX), region(LLVertexBuffer::TYPE_NORMAL),
                     region(LLVertexBuffer::TYPE_TANGENT), region(LLVertexBuffer::TYPE_WEIGHT4));
        glUniform4ui(sJob, job.mFirst, verts, job.mPalette, out.mCapacity);
        glDispatchCompute((verts + GROUP_SIZE - 1) / GROUP_SIZE, 1, 1);

        e.mFrame = frame;
        e.mFirst = job.mFirst - lo;     // where vertex 0 would be: a draw's vertex i is read at mFirst + i
        e.mSkinLo = lo;
        e.mSkinHi = lo + verts - 1;
        e.mBuffer = out.mName;
        e.mCapacity = out.mCapacity;
        e.mPaletteSerial = job.mPaletteSerial;
        e.mContentSerial = vb->getFSContentSerial();
        ++sStats.mJobs;
        sStats.mSkinned += verts;
    }
    // the passes read the copies as vertex attributes
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    for (GLuint binding = 0; binding < 3; ++binding)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, 0);
    }
    glUseProgram(LLGLSLShader::sCurBoundShader);
    // the next frame writes the other output, never the one this frame's passes read
    sPing ^= 1;
}
