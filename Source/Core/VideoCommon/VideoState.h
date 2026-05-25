// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class PointerWrap;

void VideoCommon_DoState(PointerWrap& p);

// Skips GPU readbacks (texture cache, framebuffer, bounding box) in DoState.
// Register state (BP/XF/FIFO) is always saved. Must match between save and load.
void VideoCommon_SetSkipGPUReadbackForRollback(bool skip);
bool VideoCommon_GetSkipGPUReadbackForRollback();
