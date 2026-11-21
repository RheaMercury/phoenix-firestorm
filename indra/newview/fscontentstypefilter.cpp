/**
 * @file fscontentstypefilter.cpp
 * @brief Item type filter for the Content tab of the build floater
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#include "fscontentstypefilter.h"

#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llinventoryfilter.h"
#include "llinventorytype.h"
#include "llmenubutton.h"
#include "llmenugl.h"
#include "llpanel.h"
#include "llpanelobjectinventory.h"
#include "lltextbox.h"
#include "lltoggleablemenu.h"
#include "llviewercontrol.h"
#include "llviewermenu.h" // gMenuHolder

namespace
{
    constexpr U64 inv_bit(LLInventoryType::EType type)
    {
        return 1ULL << type;
    }

    struct TypeEntry
    {
        const char* mName;  // menu item name and click parameter
        U64 mTypes;         // LLInventoryType bits shown by this entry
    };

    // One entry per bit of FSBuildContentFilterTypes, in menu order.
    // "other" is whatever the entries above it leave out.
    constexpr U64 OTHER_TYPES = ~(inv_bit(LLInventoryType::IT_OBJECT)
                                | inv_bit(LLInventoryType::IT_LSL)
                                | inv_bit(LLInventoryType::IT_NOTECARD)
                                | inv_bit(LLInventoryType::IT_TEXTURE) | inv_bit(LLInventoryType::IT_SNAPSHOT)
                                | inv_bit(LLInventoryType::IT_ANIMATION)
                                | inv_bit(LLInventoryType::IT_SOUND)
                                | inv_bit(LLInventoryType::IT_LANDMARK)
                                | inv_bit(LLInventoryType::IT_WEARABLE)
                                | inv_bit(LLInventoryType::IT_MATERIAL)
                                | inv_bit(LLInventoryType::IT_GESTURE)
                                | inv_bit(LLInventoryType::IT_SETTINGS));

    const TypeEntry TYPE_ENTRIES[] =
    {
        { "objects",    inv_bit(LLInventoryType::IT_OBJECT) },
        { "scripts",    inv_bit(LLInventoryType::IT_LSL) },
        { "notecards",  inv_bit(LLInventoryType::IT_NOTECARD) },
        { "textures",   inv_bit(LLInventoryType::IT_TEXTURE) | inv_bit(LLInventoryType::IT_SNAPSHOT) },
        { "animations", inv_bit(LLInventoryType::IT_ANIMATION) },
        { "sounds",     inv_bit(LLInventoryType::IT_SOUND) },
        { "landmarks",  inv_bit(LLInventoryType::IT_LANDMARK) },
        { "wearables",  inv_bit(LLInventoryType::IT_WEARABLE) },
        { "materials",  inv_bit(LLInventoryType::IT_MATERIAL) },
        { "gestures",   inv_bit(LLInventoryType::IT_GESTURE) },
        { "settings",   inv_bit(LLInventoryType::IT_SETTINGS) },
        { "other",      OTHER_TYPES },
    };
    constexpr S32 FILTER_TYPE_COUNT = static_cast<S32>(std::size(TYPE_ENTRIES));
    static_assert(FSContentsTypeFilter::ALL_TYPES == (1U << FILTER_TYPE_COUNT) - 1, "one bit per menu entry");

    S32 entry_index(const std::string& name)
    {
        for (S32 i = 0; i < FILTER_TYPE_COUNT; ++i)
        {
            if (name == TYPE_ENTRIES[i].mName)
            {
                return i;
            }
        }
        return -1;
    }

    bool is_all(U32 mask)
    {
        mask &= FSContentsTypeFilter::ALL_TYPES;
        return mask == 0 || mask == FSContentsTypeFilter::ALL_TYPES;
    }

    // All bits set lets the filter show items of unknown type and the folders
    // of the contents, which report themselves as unknown so that a narrowed
    // filter only keeps folders holding a match.
    U64 inventory_types_for(U32 mask)
    {
        if (is_all(mask))
        {
            return ~0ULL;
        }
        U64 types = 0;
        for (S32 i = 0; i < FILTER_TYPE_COUNT; ++i)
        {
            if (mask & (1U << i))
            {
                types |= TYPE_ENTRIES[i].mTypes;
            }
        }
        return types;
    }

    class CountItems : public LLFolderViewFunctor
    {
    public:
        void doFolder(LLFolderViewFolder* folder) override {}
        void doItem(LLFolderViewItem* item) override
        {
            ++mTotal;
            LLFolderViewModelItem* vmi = item ? item->getViewModelItem() : nullptr;
            if (vmi && vmi->passedFilter())
            {
                ++mShown;
            }
        }

        S32 mTotal = 0;
        S32 mShown = 0;
    };
}

FSContentsTypeFilter::FSContentsTypeFilter(LLPanel* panel, LLPanelObjectInventory* inventory)
:   mPanel(panel),
    mInventory(inventory)
{
    mButton = mPanel->findChild<LLMenuButton>("contents_type_filter");
    mCountText = mPanel->findChild<LLTextBox>("contents_type_filter_count");

    if (mButton)
    {
        LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
        LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
        registrar.add("FSContentsType.Toggle", [this](LLUICtrl*, const LLSD& param) { onMenuClick(param); });
        enable_registrar.add("FSContentsType.Check", [this](LLUICtrl*, const LLSD& param) { return onMenuCheck(param); });

        if (LLToggleableMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLToggleableMenu>("menu_fs_contents_type_filter.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance()))
        {
            mMenuHandle = menu->getHandle();
            mButton->setMenu(menu, LLMenuButton::MP_BOTTOM_LEFT);
        }
    }

    apply();
}

FSContentsTypeFilter::~FSContentsTypeFilter()
{
    // The menu lives in the menu holder and calls back into this object
    if (LLView* menu = mMenuHandle.get())
    {
        menu->die();
    }
}

U32 FSContentsTypeFilter::getMask() const
{
    static LLCachedControl<U32> filter_types(gSavedSettings, "FSBuildContentFilterTypes", ALL_TYPES);
    return is_all(filter_types) ? ALL_TYPES : (filter_types & ALL_TYPES);
}

void FSContentsTypeFilter::onMenuClick(const LLSD& param)
{
    const std::string name = param.asString();
    if (name == "all")
    {
        setMask(ALL_TYPES);
        return;
    }

    const S32 index = entry_index(name);
    if (index < 0)
    {
        return;
    }

    const U32 bit = 1U << index;
    U32 mask = getMask();
    // From "All", picking a type shows only that type; after that each pick adds or
    // removes one, and taking away the last one goes back to all types
    mask = (mask == ALL_TYPES) ? bit : (mask ^ bit);
    setMask(mask);
}

bool FSContentsTypeFilter::onMenuCheck(const LLSD& param) const
{
    const U32 mask = getMask();
    const std::string name = param.asString();
    if (name == "all")
    {
        return mask == ALL_TYPES;
    }

    const S32 index = entry_index(name);
    return index >= 0 && mask != ALL_TYPES && (mask & (1U << index));
}

void FSContentsTypeFilter::setMask(U32 mask)
{
    mask = is_all(mask) ? ALL_TYPES : (mask & ALL_TYPES);
    const bool was_all = (getMask() == ALL_TYPES);
    gSavedSettings.setU32("FSBuildContentFilterTypes", mask);
    apply();

    // Matches inside closed folders (one per link with Whole linkset) would stay out
    // of sight, so open the folders when the list first gets narrowed
    if (was_all && mask != ALL_TYPES && mInventory)
    {
        if (LLFolderView* root = mInventory->getRootFolder())
        {
            root->setOpenArrangeRecursively(true, LLFolderViewFolder::RECURSE_DOWN);
        }
    }
    updateCount();
}

void FSContentsTypeFilter::apply()
{
    if (mInventory)
    {
        mInventory->getFilter().setFilterObjectTypes(inventory_types_for(getMask()));
    }
    updateButtonLabel();
}

void FSContentsTypeFilter::updateButtonLabel()
{
    if (!mButton)
    {
        return;
    }

    const U32 mask = getMask();
    std::string label;
    if (mask == ALL_TYPES)
    {
        label = mPanel->getString("type_filter_all");
    }
    else
    {
        S32 count = 0;
        S32 last = -1;
        for (S32 i = 0; i < FILTER_TYPE_COUNT; ++i)
        {
            if (mask & (1U << i))
            {
                ++count;
                last = i;
            }
        }

        LLStringUtil::format_map_t args;
        if (count == 1)
        {
            // Name the type the way the menu does, so it follows the translation
            std::string type_name = TYPE_ENTRIES[last].mName;
            if (LLView* menu = mMenuHandle.get())
            {
                if (LLMenuItemGL* item = menu->findChild<LLMenuItemGL>(TYPE_ENTRIES[last].mName))
                {
                    type_name = item->getLabel();
                }
            }
            args["[TYPE]"] = type_name;
            label = mPanel->getString("type_filter_one", args);
        }
        else
        {
            args["[COUNT]"] = llformat("%d", count);
            label = mPanel->getString("type_filter_many", args);
        }
    }
    mButton->setLabel(label);
}

void FSContentsTypeFilter::updateCount()
{
    if (!mCountText)
    {
        return;
    }

    LLFolderView* root = mInventory ? mInventory->getRootFolder() : nullptr;
    const bool filtering = getMask() != ALL_TYPES || (mInventory && !mInventory->getFilter().getFilterSubString().empty());
    if (!filtering || !root || !mInventory->hasInventory())
    {
        mCountText->setText(LLStringUtil::null);
        return;
    }

    CountItems counter;
    root->applyFunctorRecursively(counter);

    LLStringUtil::format_map_t args;
    args["[SHOWN]"] = llformat("%d", counter.mShown);
    args["[TOTAL]"] = llformat("%d", counter.mTotal);
    mCountText->setText(mPanel->getString("type_filter_count", args));
}
