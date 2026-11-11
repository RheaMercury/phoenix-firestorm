/**
 * @file fsblocktimer.h
 * @brief A block timer for the periodic profile log that never adds a Tracy zone.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#ifndef FS_BLOCKTIMER_H
#define FS_BLOCKTIMER_H

#include "llfasttimer.h"

// LL_RECORD_BLOCK_TIME also opens a Tracy zone in Tracy builds, under a fixed variable name, so it
// can't share a scope with an LL_PROFILE_ZONE_*. This one only records the LLTrace block timer (two
// clock reads), whatever the profiler configuration, so it can sit next to the existing Tracy zones.
// The handle must be a file scope static (LLTrace refuses handles made after startup); it may be a
// run time choice between handles.
#define FS_RECORD_BLOCK_TIME(timer_stat) \
    const LLTrace::BlockTimer& LL_GLUE_TOKENS(fs_block_time_recorder, __LINE__)(LLTrace::timeThisBlock(timer_stat)); \
    (void)LL_GLUE_TOKENS(fs_block_time_recorder, __LINE__);

#endif // FS_BLOCKTIMER_H
