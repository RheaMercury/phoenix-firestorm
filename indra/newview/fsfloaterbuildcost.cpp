/**
 * @file fsfloaterbuildcost.cpp
 * @brief Build cost tools: a linkset's cost prim by prim, and what nearby objects and avatars cost to render
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

#include "fsfloaterbuildcost.h"

#include "fsareasearch.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llfetchedgltfmaterial.h"
#include "llfloaterreg.h"
#include "llimagegl.h"
#include "lllistcontextmenu.h"
#include "llmenugl.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltextureentry.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewermenu.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "message.h"
#include "rlvactions.h"

#include <algorithm>
#include <unordered_set>

namespace
{
    constexpr F32 LINKSET_REFRESH_SECONDS = 1.f;    // the list is read again this often, for the server's answers
    constexpr F32 SCAN_BUDGET_SECONDS = 0.002f;     // time the nearby scan may take per frame
    constexpr S32 SCAN_TIMER_STRIDE = 8;            // objects scanned between looks at the clock
    constexpr size_t TOP_TEXTURES = 5;
    constexpr F64 BYTES_PER_MB = 1024.0 * 1024.0;
    constexpr S32 MAX_OBJECTS_PER_PACKET = 255;

    typedef std::unordered_set<LLViewerTexture*> texture_set_t;

    // What's heavy here's right click menu, acting on the entry under the mouse
    class FSHeavyNearbyMenu : public LLListContextMenu
    {
    public:
        explicit FSHeavyNearbyMenu(const LLHandle<FSFloaterHeavyNearby>& floater) : mFloater(floater) {}

    protected:
        LLContextMenu* createMenu() override
        {
            LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
            LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
            const LLHandle<FSFloaterHeavyNearby> floater = mFloater;
            registrar.add("HeavyNearby.Action", [floater](LLUICtrl*, const LLSD& param)
            {
                if (FSFloaterHeavyNearby* instance = floater.get())
                {
                    instance->onMenuAction(param.asString());
                }
            });
            enable_registrar.add("HeavyNearby.Enable", [floater](LLUICtrl*, const LLSD& param)
            {
                FSFloaterHeavyNearby* instance = floater.get();
                return instance && instance->onMenuEnable(param.asString());
            });
            return createFromFile("menu_fs_heavy_nearby.xml");
        }

    private:
        LLHandle<FSFloaterHeavyNearby> mFloater;
    };

    bool parse_number(const std::string& text, F64& value)
    {
        if (text.empty())
        {
            return false;
        }
        char* end = nullptr;
        value = strtod(text.c_str(), &end);
        return end && *end == '\0';
    }

    // Sort numeric columns by value, the others as text
    S32 compare_cells(S32 column, const LLScrollListItem* a, const LLScrollListItem* b)
    {
        const LLScrollListCell* cell_a = a->getColumn(column);
        const LLScrollListCell* cell_b = b->getColumn(column);
        if (!cell_a || !cell_b)
        {
            return 0;
        }
        const std::string text_a = cell_a->getValue().asString();
        const std::string text_b = cell_b->getValue().asString();
        F64 value_a = 0.0;
        F64 value_b = 0.0;
        const bool num_a = parse_number(text_a, value_a);
        const bool num_b = parse_number(text_b, value_b);
        if (num_a && num_b)
        {
            return value_a < value_b ? -1 : (value_a > value_b ? 1 : 0);
        }
        if (num_a != num_b)
        {
            return num_a ? 1 : -1; // a pending or unknown figure below any figure
        }
        return LLStringUtil::compareDict(text_a, text_b);
    }

    std::string megabytes(S64 bytes)
    {
        return llformat("%.1f", (F64)bytes / BYTES_PER_MB);
    }

    void add_texture(LLViewerTexture* texture, texture_set_t& textures)
    {
        // the default image stands in for a map a face doesn't have
        if (texture && texture != LLViewerFetchedTexture::sDefaultImagep.get() && texture->getID().notNull())
        {
            textures.insert(texture);
        }
    }

    // The textures a prim's faces draw with: a face's GLTF material maps, else its diffuse texture and,
    // with a Blinn-Phong material, its normal and specular maps
    void collect_textures(LLViewerObject* prim, texture_set_t& textures)
    {
        const U8 num_tes = prim->getNumTEs();
        for (U8 te = 0; te < num_tes; ++te)
        {
            const LLTextureEntry* tep = prim->getTE(te);
            if (!tep)
            {
                continue;
            }
            LLFetchedGLTFMaterial* gltf = dynamic_cast<LLFetchedGLTFMaterial*>(tep->getGLTFRenderMaterial());
            if (gltf)
            {
                for (LLViewerFetchedTexture* texture : { gltf->mBaseColorTexture.get(), gltf->mNormalTexture.get(),
                                                         gltf->mMetallicRoughnessTexture.get(), gltf->mEmissiveTexture.get() })
                {
                    add_texture(texture, textures);
                }
                continue;
            }
            add_texture(prim->getTEImage(te), textures);
            if (tep->getMaterialParams().notNull())
            {
                add_texture(prim->getTENormalMap(te), textures);
                add_texture(prim->getTESpecularMap(te), textures);
            }
        }
    }

    FSFloaterHeavyNearby::TextureInfo measure_texture(LLViewerTexture* texture)
    {
        FSFloaterHeavyNearby::TextureInfo info;
        info.mID = texture->getID();
        info.mWidth = texture->getFullWidth();
        info.mHeight = texture->getFullHeight();

        S32 components = 4;
        LLImageGL* image = texture->getGLTexture();
        // A texture still loading has no format yet, and sizing format 0 is a fatal error
        if (image && image->getPrimaryFormat() != 0)
        {
            if (image->getComponents() > 0)
            {
                components = image->getComponents();
            }
            if (texture->hasGLTexture())
            {
                info.mLoadedBytes = image->getMipBytes(-1);
            }
        }
        if (info.mWidth > 0 && info.mHeight > 0)
        {
            // uncompressed, with a third more for the mipmaps
            info.mFullBytes = (S64)info.mWidth * (S64)info.mHeight * (S64)components * 4 / 3;
        }
        return info;
    }

    // Sum the textures' memory and keep the biggest
    void measure_textures(const texture_set_t& textures, S64& full_bytes, S64& loaded_bytes,
                          std::vector<FSFloaterHeavyNearby::TextureInfo>& top)
    {
        std::vector<FSFloaterHeavyNearby::TextureInfo> infos;
        infos.reserve(textures.size());
        for (LLViewerTexture* texture : textures)
        {
            infos.push_back(measure_texture(texture));
            full_bytes += infos.back().mFullBytes;
            loaded_bytes += infos.back().mLoadedBytes;
        }
        const size_t count = std::min(TOP_TEXTURES, infos.size());
        std::partial_sort(infos.begin(), infos.begin() + count, infos.end(),
            [](const FSFloaterHeavyNearby::TextureInfo& a, const FSFloaterHeavyNearby::TextureInfo& b)
            {
                return a.mFullBytes != b.mFullBytes ? a.mFullBytes > b.mFullBytes : a.mLoadedBytes > b.mLoadedBytes;
            });
        top.assign(infos.begin(), infos.begin() + count);
    }

    struct Measure
    {
        S32             mPrims = 0;
        U32             mTriangles = 0;
        texture_set_t   mTextures;
    };

    void measure_prim(LLViewerObject* prim, Measure& measure)
    {
        if (!prim || prim->isDead() || prim->isAvatar())
        {
            return;
        }
        ++measure.mPrims;
        S32 vertices = 0;
        measure.mTriangles += prim->getTriangleCount(&vertices);
        collect_textures(prim, measure.mTextures);
    }

    void measure_linkset(LLViewerObject* root, Measure& measure)
    {
        measure_prim(root, measure);
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            measure_prim(child, measure);   // a seated avatar is a child too, and is left out
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// FSFloaterLinksetCost

FSFloaterLinksetCost::FSFloaterLinksetCost(const LLSD& key)
:   LLFloater(key)
{
}

bool FSFloaterLinksetCost::postBuild()
{
    mList = getChild<LLScrollListCtrl>("prims");
    mTotals = getChild<LLTextBox>("totals");
    mStatus = getChild<LLTextBox>("status");
    mSelectPrimBtn = getChild<LLUICtrl>("select_prim");

    mList->setSortCallback([](S32 column, const LLScrollListItem* a, const LLScrollListItem* b) { return compare_cells(column, a, b); });
    mList->setDoubleClickCallback([this]() { onClickSelectPrim(); });
    mList->sortByColumn("li", false);

    mSelectPrimBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickSelectPrim(); });
    getChild<LLUICtrl>("refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRefresh(); });
    return true;
}

void FSFloaterLinksetCost::onOpen(const LLSD& key)
{
    // As the Inspect window does: hold the selection, so it isn't dropped when the menu that opened
    // this window closes
    mSelection = LLSelectMgr::getInstance()->getSelection();
    refreshList();
}

void FSFloaterLinksetCost::onClose(bool app_quitting)
{
    mSelection.clear();
}

LLViewerObject* FSFloaterLinksetCost::selectedRoot() const
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLViewerObject* object = selection->getPrimaryObject();
    if (!object || object->isDead() || !selection->contains(object))
    {
        object = selection->getFirstObject();
    }
    if (!object || object->isDead())
    {
        return nullptr;
    }
    LLViewerObject* root = object->getRootEdit();
    if (!root || root->isDead() || root->isAvatar())
    {
        return nullptr;
    }
    return root;
}

void FSFloaterLinksetCost::draw()
{
    bool refresh = mRefreshTimer.getElapsedTimeF32() >= LINKSET_REFRESH_SECONDS;
    if (mServerDirty)
    {
        mServerDirty = false;
        refresh = true;
    }

    // follow the selection to another linkset; with nothing selected, keep showing the last one
    if (LLViewerObject* root = selectedRoot())
    {
        if (root->getID() != mRootID)
        {
            mRootID = root->getID();
            mNames.clear();
            mList->deleteAllItems();
            requestServerCost(root);
            refresh = true;
        }
    }

    if (refresh)
    {
        refreshList();
    }
    mSelectPrimBtn->setEnabled(mList->getFirstSelected() != nullptr);
    LLFloater::draw();
}

std::string FSFloaterLinksetCost::primName(LLViewerObject* prim, S32 link_num)
{
    // The viewer only knows a prim's name once it was selected; keep it for when it no longer is
    LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(prim);
    if (node && node->mValid && !node->mName.empty())
    {
        mNames[prim->getID()] = node->mName;
    }
    auto found = mNames.find(prim->getID());
    if (found != mNames.end())
    {
        return found->second;
    }
    LLStringUtil::format_map_t args;
    args["LINK"] = llformat("%d", link_num);
    return getString("unnamed_prim", args);
}

void FSFloaterLinksetCost::refreshList()
{
    mRefreshTimer.reset();

    LLViewerObject* root = mRootID.notNull() ? gObjectList.findObject(mRootID) : nullptr;
    if (!root || root->isDead())
    {
        mList->deleteAllItems();
        mTotals->setText(LLStringUtil::null);
        mStatus->setText(getString(mRootID.isNull() ? "status_none" : "status_gone"));
        return;
    }

    // the root first, then the children in the order the viewer keeps them
    std::vector<LLViewerObject*> prims;
    prims.push_back(root);
    for (const LLPointer<LLViewerObject>& child : root->getChildren())
    {
        if (child && !child->isDead() && !child->isAvatar())
        {
            prims.push_back(child);
        }
    }
    const bool linked = prims.size() > 1;

    const LLUUID selected = mList->getCurrentID();
    const S32 scroll = mList->getScrollPos();
    mList->deleteAllItems();

    const std::string pending = getString("pending");
    texture_set_t all_textures;
    F32 total_download = 0.f;
    U64 total_triangles = 0;
    U64 total_vertices = 0;
    bool any_pending = false;

    for (size_t i = 0; i < prims.size(); ++i)
    {
        LLViewerObject* prim = prims[i];
        const S32 link_num = linked ? (S32)i + 1 : 0;   // as scripts count: 0 unlinked, 1 the root

        // Asking for a cost the server hasn't sent yet queues a request for it (ObjectCost capability)
        const F32 object_cost = prim->getObjectCost();
        const F32 physics_cost = prim->getPhysicsCost();
        const bool cost_known = object_cost > 0.f;  // every prim weighs something once the server has said
        any_pending |= !cost_known;

        const F32 download = prim->getStreamingCost();
        total_download += download;

        S32 vertices = 0;
        const U32 triangles = prim->getTriangleCount(&vertices);
        total_triangles += triangles;
        total_vertices += (U64)llmax(vertices, 0);

        texture_set_t textures;
        collect_textures(prim, textures);
        all_textures.insert(textures.begin(), textures.end());

        LLSD row;
        row["id"] = prim->getID();
        LLSD& columns = row["columns"];
        columns[0]["column"] = "link";
        columns[0]["value"] = llformat("%d", link_num);
        columns[1]["column"] = "name";
        columns[1]["value"] = primName(prim, link_num);
        columns[2]["column"] = "li";
        columns[2]["value"] = cost_known ? llformat("%.3f", object_cost) : pending;
        columns[3]["column"] = "download";
        columns[3]["value"] = llformat("%.3f", download);
        columns[4]["column"] = "physics";
        columns[4]["value"] = cost_known ? llformat("%.3f", physics_cost) : pending;
        columns[5]["column"] = "triangles";
        columns[5]["value"] = llformat("%u", triangles);
        columns[6]["column"] = "vertices";
        columns[6]["value"] = llformat("%d", vertices);
        columns[7]["column"] = "textures";
        columns[7]["value"] = llformat("%d", (S32)textures.size());
        mList->addElement(row);
    }
    mList->updateSort();
    if (selected.notNull())
    {
        mList->selectByID(selected);
    }
    mList->setScrollPos(scroll);

    const F32 linkset_cost = root->getLinksetCost();
    const bool linkset_known = linkset_cost > 0.f;
    std::string server = getString("not_available");
    if (mServerState == SERVER_KNOWN)
    {
        server = llformat("%.2f", mServerCost);
    }
    else if (mServerState == SERVER_PENDING)
    {
        server = pending;
    }

    LLStringUtil::format_map_t args;
    args["LI"] = linkset_known ? llformat("%.2f", linkset_cost) : pending;
    args["DOWNLOAD"] = llformat("%.2f", total_download);
    args["PHYSICS"] = linkset_known ? llformat("%.2f", root->getLinksetPhysicsCost()) : pending;
    args["SERVER"] = server;
    args["TRIS"] = llformat("%llu", (unsigned long long)total_triangles);
    args["VERTS"] = llformat("%llu", (unsigned long long)total_vertices);
    args["TEXTURES"] = llformat("%d", (S32)all_textures.size());
    mTotals->setText(getString("totals", args));

    LLStringUtil::format_map_t status_args;
    status_args["COUNT"] = llformat("%d", (S32)prims.size());
    mStatus->setText(getString((any_pending || !linkset_known) ? "status_pending" : "status", status_args));
}

void FSFloaterLinksetCost::requestServerCost(LLViewerObject* root)
{
    mServerState = SERVER_NONE;
    LLViewerRegion* region = root ? root->getRegion() : nullptr;
    if (!region || !region->capabilitiesReceived())
    {
        return;
    }
    const std::string url = region->getCapability("ResourceCostSelected");
    if (url.empty())
    {
        return;
    }

    // as the Object Weights window asks for it
    LLAccountingCostManager::getInstance()->addObject(root->getID());
    generateTransactionID();
    LLAccountingCostManager::getInstance()->fetchCosts(Roots, url, getObserverHandle());
    mServerState = SERVER_PENDING;
}

// virtual
void FSFloaterLinksetCost::onWeightsUpdate(const SelectionCost& selection_cost)
{
    // from a coroutine: only keep the figure, the list is redrawn in draw()
    mServerCost = selection_cost.mSimulationCost;
    mServerState = SERVER_KNOWN;
    mServerDirty = true;
}

// virtual
void FSFloaterLinksetCost::setErrorStatus(S32 status, const std::string& reason)
{
    LL_WARNS() << "Linkset server cost request failed: " << status << " " << reason << LL_ENDL;
    mServerState = SERVER_NONE;
    mServerDirty = true;
}

// virtual
void FSFloaterLinksetCost::generateTransactionID()
{
    mTransactionID.generate();
}

void FSFloaterLinksetCost::onClickSelectPrim()
{
    LLViewerObject* prim = gObjectList.findObject(mList->getCurrentID());
    if (!prim || prim->isDead())
    {
        return;
    }

    // as Edit Linked Parts does: select only that prim of the linkset
    if (!gSavedSettings.getBOOL("EditLinkedParts"))
    {
        gSavedSettings.setBOOL("EditLinkedParts", true);
    }
    LLSelectMgr::getInstance()->deselectAll();
    LLSelectMgr::getInstance()->selectObjectOnly(prim);
}

void FSFloaterLinksetCost::onClickRefresh()
{
    if (LLViewerObject* root = mRootID.notNull() ? gObjectList.findObject(mRootID) : nullptr)
    {
        requestServerCost(root);
    }
    refreshList();
}

///////////////////////////////////////////////////////////////////////////////
// FSFloaterHeavyNearby

FSFloaterHeavyNearby::FSFloaterHeavyNearby(const LLSD& key)
:   LLFloater(key)
{
}

FSFloaterHeavyNearby::~FSFloaterHeavyNearby()
{
}

bool FSFloaterHeavyNearby::postBuild()
{
    mList = getChild<LLScrollListCtrl>("entries");
    mTextureList = getChild<LLScrollListCtrl>("textures");
    mRange = getChild<LLSpinCtrl>("range");
    mRegionOnly = getChild<LLUICtrl>("region_only");
    mBeacon = getChild<LLCheckBoxCtrl>("beacon");
    mStatus = getChild<LLTextBox>("status");

    mList->setSortCallback([](S32 column, const LLScrollListItem* a, const LLScrollListItem* b) { return compare_cells(column, a, b); });
    mList->sortByColumn("full_mb", false);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshTextures(); });
    mList->setDoubleClickCallback([this]() { onDoubleClick(); });
    mMenu = std::make_unique<FSHeavyNearbyMenu>(getDerivedHandle<FSFloaterHeavyNearby>());
    // the list calls this only on a row: select it, then the menu acts on it
    mList->setRightMouseDownCallback([this](LLUICtrl*, S32 x, S32 y, MASK)
    {
        LLScrollListItem* item = mList->hitItem(x, y);
        if (!item)
        {
            return;
        }
        if (!item->getSelected())
        {
            mList->deselectAllItems(true);
            mList->selectByID(item->getUUID());
            refreshTextures();
        }
        mMenu->show(mList, uuid_vec_t{ item->getUUID() }, x, y);
    });

    // the draw distance by default
    mRange->setValue(LLSD((F64)gSavedSettings.getF32("RenderFarClip")));

    getChild<LLUICtrl>("refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { startScan(); });
    return true;
}

void FSFloaterHeavyNearby::onOpen(const LLSD& key)
{
    if (!mScanning)
    {
        startScan();
    }
}

void FSFloaterHeavyNearby::onClose(bool app_quitting)
{
    // stop a scan running: its idle callback ends at its next call
    ++mScanGeneration;
    mScanning = false;
    mScanEntries.clear();
}

void FSFloaterHeavyNearby::startScan()
{
    ++mScanGeneration;
    mScanning = true;
    mScanIndex = 0;
    mScanEntries.clear();
    mScanCenter = gAgent.getPositionGlobal();
    mScanRange = llmax(mRange->getValueF32(), 1.f);
    mScanRegionOnly = mRegionOnly->getValue().asBoolean();
    mStatus->setText(getString("scanning"));

    const U32 generation = mScanGeneration;
    LLHandle<FSFloaterHeavyNearby> handle = getDerivedHandle<FSFloaterHeavyNearby>();
    doOnIdleRepeating([handle, generation]()
    {
        FSFloaterHeavyNearby* floater = handle.get();
        return !floater || floater->scanStep(generation);
    });
}

bool FSFloaterHeavyNearby::scanStep(U32 generation)
{
    if (generation != mScanGeneration || !mScanning)
    {
        return true;    // replaced by a newer scan, or the window closed
    }

    // A slice of the object list per frame. The list can change between frames; an object missed or
    // seen twice is harmless, as only roots and avatars are measured and each from scratch.
    LLTimer budget;
    S32 stride = 0;
    while (budget.getElapsedTimeF32() < SCAN_BUDGET_SECONDS || stride != 0)
    {
        if (mScanIndex >= gObjectList.getNumObjects())
        {
            finishScan();
            return true;
        }
        scanObject(gObjectList.getObject(mScanIndex++));
        stride = (stride + 1) % SCAN_TIMER_STRIDE;
    }

    LLStringUtil::format_map_t args;
    const S32 total = llmax(gObjectList.getNumObjects(), 1);
    args["PERCENT"] = llformat("%d", llclamp(mScanIndex * 100 / total, 0, 100));
    mStatus->setText(getString("scanning_progress", args));
    return false;
}

void FSFloaterHeavyNearby::scanObject(LLViewerObject* object)
{
    if (!object || object->isDead())
    {
        return;
    }
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (mScanRegionOnly && object->getRegion() != agent_region)
    {
        return;
    }
    const F32 distance = (F32)(object->getPositionGlobal() - mScanCenter).magVec();
    if (distance > mScanRange)
    {
        return;
    }

    if (object->isAvatar())
    {
        LLVOAvatar* avatar = object->asAvatar();
        // an animated object's avatar: its linkset is listed as an object
        if (!avatar || avatar->isControlAvatar())
        {
            return;
        }
        Entry entry;
        entry.mID = avatar->getID();
        entry.mIsAvatar = true;
        entry.mDistance = distance;
        measureAvatar(avatar, entry);
        mScanEntries.push_back(std::move(entry));
        return;
    }

    // Linksets once, by their root. A child, and an attachment (its parent is an avatar), have a parent;
    // attachments are counted with their avatar.
    if (object->getParent() || object->getPCode() != LL_PCODE_VOLUME)
    {
        return;
    }

    Measure measure;
    measure_linkset(object, measure);

    Entry entry;
    entry.mID = object->getID();
    entry.mDistance = distance;
    entry.mName = objectName(object);
    entry.mPrims = measure.mPrims;
    entry.mTriangles = measure.mTriangles;
    entry.mTextures = (S32)measure.mTextures.size();
    measure_textures(measure.mTextures, entry.mFullBytes, entry.mLoadedBytes, entry.mTopTextures);
    mScanEntries.push_back(std::move(entry));
}

void FSFloaterHeavyNearby::measureAvatar(LLVOAvatar* avatar, Entry& entry)
{
    LLAvatarName av_name;
    if (LLAvatarNameCache::get(avatar->getID(), &av_name))
    {
        entry.mName = av_name.getCompleteName();
    }
    else
    {
        entry.mName = avatar->getFullname();
    }
    entry.mComplexity = (S32)avatar->getVisualComplexity();

    Measure measure;
    for (const auto& point : avatar->mAttachmentPoints)
    {
        const LLViewerJointAttachment* attachment = point.second;
        if (!attachment)
        {
            continue;
        }
        for (const LLPointer<LLViewerObject>& attached : attachment->mAttachedObjects)
        {
            if (attached && !attached->isDead())
            {
                measure_linkset(attached, measure);
            }
        }
    }

    entry.mPrims = measure.mPrims;
    entry.mTriangles = measure.mTriangles;
    entry.mTextures = (S32)measure.mTextures.size();
    measure_textures(measure.mTextures, entry.mFullBytes, entry.mLoadedBytes, entry.mTopTextures);
}

void FSFloaterHeavyNearby::finishScan()
{
    mScanning = false;
    mEntries.swap(mScanEntries);
    mScanEntries.clear();
    refreshList();

    S32 avatars = 0;
    for (const Entry& entry : mEntries)
    {
        avatars += entry.mIsAvatar ? 1 : 0;
    }
    LLStringUtil::format_map_t args;
    args["OBJECTS"] = llformat("%d", (S32)mEntries.size() - avatars);
    args["AVATARS"] = llformat("%d", avatars);
    args["RANGE"] = llformat("%.0f", mScanRange);
    mStatus->setText(getString(mScanRegionOnly ? "status_region" : "status", args));
    requestNames();
}

void FSFloaterHeavyNearby::requestNames()
{
    // The names of the objects listed without one, asked for the way area search does: selecting them
    // on the simulator and deselecting them at once, which answers with their properties. Objects the
    // agent has selected are left alone, so their selection stays.
    std::map<LLViewerRegion*, std::vector<U32>> by_region;
    for (const Entry& entry : mEntries)
    {
        if (entry.mIsAvatar || mNames.count(entry.mID) || mNamesAsked.count(entry.mID))
        {
            continue;
        }
        LLViewerObject* object = gObjectList.findObject(entry.mID);
        if (!object || object->isDead() || !object->getRegion() || object->isSelected())
        {
            continue;
        }
        mNamesAsked.insert(entry.mID);
        by_region[object->getRegion()].push_back(object->getLocalID());
    }

    LLMessageSystem* msg = gMessageSystem;
    for (const auto& [region, local_ids] : by_region)
    {
        for (bool select : { true, false })
        {
            S32 blocks = 0;
            for (U32 local_id : local_ids)
            {
                if (blocks == 0)
                {
                    msg->newMessageFast(select ? _PREHASH_ObjectSelect : _PREHASH_ObjectDeselect);
                    msg->nextBlockFast(_PREHASH_AgentData);
                    msg->addUUIDFast(_PREHASH_AgentID, gAgentID);
                    msg->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
                }
                msg->nextBlockFast(_PREHASH_ObjectData);
                msg->addU32Fast(_PREHASH_ObjectLocalID, local_id);
                if (++blocks >= MAX_OBJECTS_PER_PACKET || msg->isSendFull(nullptr))
                {
                    msg->sendReliable(region->getHost());
                    blocks = 0;
                }
            }
            if (blocks > 0)
            {
                msg->sendReliable(region->getHost());
            }
        }
    }
}

// static
void FSFloaterHeavyNearby::processObjectProperties(LLMessageSystem* msg)
{
    FSFloaterHeavyNearby* floater = LLFloaterReg::findTypedInstance<FSFloaterHeavyNearby>("fs_heavy_nearby");
    if (!floater || floater->mNamesAsked.empty())
    {
        return;
    }
    const S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
    for (S32 i = 0; i < count; ++i)
    {
        LLUUID id;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id, i);
        if (floater->mNamesAsked.erase(id))
        {
            std::string name;
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, name, i);
            floater->receiveName(id, name);
        }
    }
}

void FSFloaterHeavyNearby::receiveName(const LLUUID& id, const std::string& name)
{
    if (name.empty())
    {
        return;
    }
    mNames[id] = name;
    for (Entry& entry : mEntries)
    {
        if (entry.mID == id)
        {
            entry.mName = name;
            break;
        }
    }
    if (LLScrollListItem* item = mList->getItem(LLSD(id)))
    {
        if (LLScrollListCell* cell = item->getColumn(mList->getColumn("name")->mIndex))
        {
            cell->setValue(LLSD(name));
        }
    }
}

void FSFloaterHeavyNearby::draw()
{
    LLFloater::draw();

    // a beacon on the selected entry, with its name
    if (!mBeacon || !mBeacon->get())
    {
        return;
    }
    const Entry* entry = findEntry(mList->getCurrentID());
    LLViewerObject* object = entry ? gObjectList.findObject(entry->mID) : nullptr;
    if (!object || object->isDead())
    {
        return;
    }
    static LLCachedControl<S32> beacon_line_width(gSavedSettings, "DebugBeaconLineWidth");
    static LLUIColor beacon_color = LLUIColorTable::getInstance()->getColor("AreaSearchBeaconColor");
    static LLUIColor beacon_text_color = LLUIColorTable::getInstance()->getColor("PathfindingDefaultBeaconTextColor");
    gObjectList.addDebugBeacon(object->getPositionAgent(), entry->mName, beacon_color, beacon_text_color, beacon_line_width);
}

bool FSFloaterHeavyNearby::onMenuEnable(const std::string& action)
{
    LLViewerObject* object = gObjectList.findObject(mList->getCurrentID());
    if (!object || object->isDead())
    {
        return false;
    }
    if (action == "look")
    {
        return true;
    }
    // editing and the cost breakdown are for objects; an avatar's attachments can't be edited from here
    if (object->isAvatar())
    {
        return false;
    }
    if (action == "edit" || action == "costs")
    {
        return !RlvActions::isRlvEnabled() || RlvActions::canEdit(object);
    }
    return false;
}

void FSFloaterHeavyNearby::onMenuAction(const std::string& action)
{
    if (!onMenuEnable(action))
    {
        return;
    }
    if (action == "look")
    {
        onDoubleClick();
        return;
    }
    LLViewerObject* object = gObjectList.findObject(mList->getCurrentID());
    LLSelectMgr::getInstance()->deselectAll();
    LLSelectMgr::getInstance()->selectObjectAndFamily(object);
    if (action == "edit")
    {
        handle_object_edit();
    }
    else if (action == "costs")
    {
        LLFloaterReg::showInstance("fs_linkset_cost");
    }
}

void FSFloaterHeavyNearby::refreshList()
{
    const LLUUID selected = mList->getCurrentID();
    mList->deleteAllItems();

    const std::string kind_avatar = getString("kind_avatar");
    const std::string kind_object = getString("kind_object");
    for (const Entry& entry : mEntries)
    {
        LLSD row;
        row["id"] = entry.mID;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "name";
        columns[0]["value"] = entry.mName;
        columns[1]["column"] = "kind";
        columns[1]["value"] = entry.mIsAvatar ? kind_avatar : kind_object;
        columns[2]["column"] = "distance";
        columns[2]["value"] = llformat("%.0f", entry.mDistance);
        columns[3]["column"] = "full_mb";
        columns[3]["value"] = megabytes(entry.mFullBytes);
        columns[4]["column"] = "loaded_mb";
        columns[4]["value"] = megabytes(entry.mLoadedBytes);
        columns[5]["column"] = "triangles";
        columns[5]["value"] = llformat("%u", entry.mTriangles);
        columns[6]["column"] = "textures";
        columns[6]["value"] = llformat("%d", entry.mTextures);
        columns[7]["column"] = "prims";
        columns[7]["value"] = llformat("%d", entry.mPrims);
        columns[8]["column"] = "complexity";
        columns[8]["value"] = entry.mComplexity >= 0 ? llformat("%d", entry.mComplexity) : std::string("-");
        mList->addElement(row);
    }
    mList->updateSort();
    if (selected.notNull())
    {
        mList->selectByID(selected);
    }
    refreshTextures();
}

const FSFloaterHeavyNearby::Entry* FSFloaterHeavyNearby::findEntry(const LLUUID& id) const
{
    if (id.isNull())
    {
        return nullptr;
    }
    for (const Entry& entry : mEntries)
    {
        if (entry.mID == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

void FSFloaterHeavyNearby::refreshTextures()
{
    mTextureList->deleteAllItems();
    const Entry* entry = findEntry(mList->getCurrentID());
    if (!entry)
    {
        return;
    }

    // biggest first, as measured; the list doesn't sort. Textures are listed by rank only: their
    // UUIDs are not shown, since the viewer must not hand out the IDs of textures in other people's
    // content.
    S32 rank = 0;
    for (const TextureInfo& info : entry->mTopTextures)
    {
        LLSD row;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "texture";
        LLStringUtil::format_map_t args;
        args["RANK"] = llformat("%d", ++rank);
        columns[0]["value"] = getString("texture_rank", args);
        columns[1]["column"] = "resolution";
        columns[1]["value"] = (info.mWidth > 0 && info.mHeight > 0) ? llformat("%dx%d", info.mWidth, info.mHeight)
                                                                    : getString("unknown_size");
        columns[2]["column"] = "full_mb";
        columns[2]["value"] = megabytes(info.mFullBytes);
        columns[3]["column"] = "loaded_mb";
        columns[3]["value"] = megabytes(info.mLoadedBytes);
        mTextureList->addElement(row);
    }
}

std::string FSFloaterHeavyNearby::objectName(LLViewerObject* object) const
{
    // The viewer only knows an object's name once it has asked for its properties: when it is selected,
    // area search has looked at it, or this window asked after an earlier scan (requestNames)
    if (auto found = mNames.find(object->getID()); found != mNames.end())
    {
        return found->second;
    }
    if (LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(object))
    {
        if (node->mValid && !node->mName.empty())
        {
            return node->mName;
        }
    }
    if (FSAreaSearch* area_search = LLFloaterReg::findTypedInstance<FSAreaSearch>("area_search"))
    {
        auto found = area_search->mObjectDetails.find(object->getID());
        if (found != area_search->mObjectDetails.end()
            && found->second.request == FSObjectProperties::FINISHED
            && !found->second.name.empty())
        {
            return found->second.name;
        }
    }
    LLStringUtil::format_map_t args;
    args["ID"] = object->getID().asString().substr(0, 8);
    return getString("unnamed_object", args);
}

void FSFloaterHeavyNearby::onDoubleClick()
{
    const LLUUID id = mList->getCurrentID();
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object || object->isDead())
    {
        mStatus->setText(getString("object_gone"));
        return;
    }
    // look at it
    gAgentCamera.setFocusOnAvatar(false, ANIMATE);
    gAgentCamera.setFocusGlobal(object->getPositionGlobal(), id);
}
