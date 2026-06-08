// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class CPUThreadGuard;
}

namespace HLE_Misc
{
void UnimplementedFunction(const Core::CPUThreadGuard& guard);
void HBReload(const Core::CPUThreadGuard& guard);
void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard);
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetGameLoop(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetGameProcCallsite(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetClearPadEdgeCallsite(const Core::CPUThreadGuard& guard);
}  // namespace HLE_Misc
