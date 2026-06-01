// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class PointerWrap;

void VideoCommon_DoState(PointerWrap& p);

// Skips GPU readbacks (texture cache, framebuffer, bounding box) in DoState.
// Register state (BP/XF/FIFO) is always saved. Must match between save and load.
void VideoCommon_SetSkipGPUReadbackForRollback(bool skip);
bool VideoCommon_GetSkipGPUReadbackForRollback();

// Skips the VertexManager::Flush() call at the start of DoState during rollback.
// The flush submits a GPU draw which is wasted work when we're about to re-simulate the frame.
void VideoCommon_SetSkipVertexFlushForRollback(bool skip);
bool VideoCommon_GetSkipVertexFlushForRollback();
