#include "Savestate.h"

#include "Core/HW/Memmap.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/System.h"
#include <thread>
#include "VideoCommon/OnScreenDisplay.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "MemRegions.h"

#define LOW_BOUND_MEM 0x80000000


// lots of code here is heavily derived from Slippi's Savestates.cpp

BrawlbackSavestate::BrawlbackSavestate(Core::System& system) : m_system(&system)
{
    // init member list with proper addresses
    initBackupLocs();
    // iterate through address ranges and allocate mem for our savestates
    for (auto it = backupLocs.begin(); it != backupLocs.end(); ++it) {
        auto size = it->endAddress - it->startAddress;
        it->data = static_cast<u8*>(Common::AllocateAlignedMemory(size, 64));
    }
}

BrawlbackSavestate::~BrawlbackSavestate()
{
    for (auto it = backupLocs.begin(); it != backupLocs.end(); ++it)
    {
        Common::FreeAlignedMemory(it->data);
    }
}


void BrawlbackSavestate::getDolphinState(PointerWrap& p)
{
    // p.DoArray(Memory::m_pRAM, Memory::RAM_SIZE);
    // p.DoMarker("Memory");
    // VideoInterface::DoState(p);
    // p.DoMarker("VideoInterface");
    // SerialInterface::DoState(p);
    // p.DoMarker("SerialInterface");
    // ProcessorInterface::DoState(p);
    // p.DoMarker("ProcessorInterface");
    // DSP::DoState(p);
    // p.DoMarker("DSP");
    // DVDInterface::DoState(p);
    // p.DoMarker("DVDInterface");
    // GPFifo::DoState(p);
    // p.DoMarker("GPFifo");
    m_system->GetExpansionInterface().DoState(p);
    p.DoMarker("ExpansionInterface");
    // AudioInterface::DoState(p);
    // p.DoMarker("AudioInterface");
}

void BrawlbackSavestate::initBackupLocs()
{
    // https://docs.google.com/spreadsheets/d/1xVvcsGZg930uVhIawacDp-brbNpLJQtzj3ry-ZQaXWo/edit?usp=sharing
    static std::vector<ssBackupLoc> allMem = {
        {0x805b5160, 0x817da5a0, nullptr, "mem1"},  // all of mem1
        {0x90000800, 0x935e0000, nullptr, "mem2"},  // all of mem2
    };

    static std::vector<ssBackupLoc> fullBackupRegions = {
        // {start address, end address, nullptr},

        // data/bss sections
                
        /*
        {0x800064E0, 0x800064E0 + 0x3280, nullptr}, // 0
        {0x80009760, 0x80009760 + 0x3100, nullptr}, // 1
        {0x804064E0, 0x804064E0 + 0x300, nullptr}, // 2
        {0x804067E0, 0x804067E0 + 0x20, nullptr}, // 3
        {0x80406800, 0x80406800 + 0x19E80, nullptr}, // 4
        {0x80420680, 0x80420680 + 0x741C0, nullptr}, // 5
        {0x8059C420, 0x8059C420 + 0x3B60, nullptr}, // 6
        {0x805A1320, 0x805A1320 + 0x3E00, nullptr}, // 7 
        */
        
        // data sections 8-10 are size 0

        // bss

        //{0x80494880, 0x8059c41f, nullptr}, // MAIN_uninitialized0
        //{0x8059ff80, 0x805a131f, nullptr}, // MAIN_uninitialized1
        //{0x805a5120, 0x805a5153, nullptr}, // MAIN_uninitialized2

        //{0x80494880, 0x80494880 + 0x1108D4}, // bss  0x80494880 - 0x805A5154  (according to brawlcrate)
        


        //{0x80001800, 0x80003000, nullptr}, // default gecko code region
        
        // mem1
        {0x805b8a00, 0x805b8a00+0x17c, nullptr, "gfTaskScheduler"}, // gfTaskScheduler (fixes special move crashes)
        
        //{0x805b5160, 0x805ca260, nullptr, "SystemFW"}, // System FW
        {0x80611f60, 0x80673460, nullptr, "System"}, // System
        {0x80b8db60, 0x80c23a60, nullptr, "Effect"}, // Effect
        //{0x805d1e60, 0x80611f60, nullptr, "RenderFIFO"}, // RenderFifo
        //{0x80c23a60, 0x80da3a60, nullptr, "InfoResource"}, // InfoResource
        //{0x815edf60, 0x817bad60, nullptr, "InfoExtraResource"}, // InfoExtraResource
        //{0x80da3a60, 0x80fd6260, nullptr, "CommonResource"}, // CommonResource
        //{0x81049e60, 0x81061060, nullptr, "Tmp"}, // Tmp
        //{0x8154e560, 0x81601960, nullptr, "Physics"}, // Physics
        //{0x81382b60, 0x814ce460, nullptr, "ItemInstance"}, // ItemInstance
        //{0x814ce460, 0x8154e560, nullptr, "StageInstance"}, // StageInstance
        {0x8123ab60, 0x8128cb60, nullptr, "Fighter1Instance"}, // Fighter1Instance
        {0x8128cb60, 0x812deb60, nullptr, "Fighter2Instance"}, // Fighter2Instance
        //{0x812deb60, 0x81330b60, nullptr, "Fighter3Instance"}, // Fighter3Instance
        //{0x81330b60, 0x81382b60, nullptr, "Fighter4Instance"}, // Fighter4Instance
        {0x81601960, 0x81734d60, nullptr, "InfoInstance"}, // InfoInstance
        {0x81734d60, 0x817ce860, nullptr, "MenuInstance"}, // MenuInstance
        //{0x80fd6260, 0x81049e60, nullptr, "MeleeFont"}, // MeleeFont

        // overlaycommon
        // halfway point 0x809007E0


        //{0x80673460, 0x80b8db60, nullptr, "OverlayCommon"}, // OverlayCommon 5mb
        //{0x80673460, 0x80673460+0x28D380, nullptr, "OverlayCommon first half"}, // OverlayCommon first half
        //{/*0x80673460+0x28D380*/0x809007E0, 0x80b8db60, nullptr, "OverlayCommon second half"}, // OverlayCommon second half

        //{/*0x80673460+0x28D380*/0x809007E0, 0x809007E0+0x1469C0, nullptr, "OverlayCommon 3/4"}, // OverlayCommon 3/4
        {/*0x80673460+0x28D380*/0x809007E0+0x1469C0, 0x80b8db60, nullptr, "OverlayCommon 4/4"}, // OverlayCommon 4/4

        //{0x810f1a60, 0x81162560, nullptr, "OverlayStage"}, // OverlayStage
        //{0x81162560, 0x811aa160, nullptr, "OverlayMenu"}, // OverlayMenu
        //{0x81061060, 0x810a9560, nullptr, "OverlayFighter1"}, // OverlayFighter1
        //{0x810a9560, 0x810f1a60, nullptr, "OverlayFighter2"}, // OverlayFighter2
        //{0x811aa160, 0x811f2660, nullptr, "OverlayFighter3"}, // OverlayFighter3
        //{0x811f2660, 0x8123ab60, nullptr, "OverlayFighter4"}, // OverlayFighter4
        //{0x805ca260, 0x805d1e60, nullptr, "Thread"}, // Thread

        // mem2

        //{0x90199800, 0x90e61400, nullptr, "Sound"}, // Sound
        //{0x90e77500, 0x90fddc00, nullptr, "Network"}, // Network
        {0x90e61400, 0x90e77500, nullptr, "WiiPad"}, // WiiPad
        //{0x91018b00, 0x91301b00, nullptr, "IteamResource"}, // IteamResource
        //{0x91301b00, 0x9134cc00, nullptr, "Replay"}, // Replay
        //{0x92f34700, 0x9359ae00, nullptr, "StageResource"}, // StageResource







        //{0x9151fa00, 0x91a72e00, nullptr, "Fighter1Resource"}, // Fighter1Resource  (crashes without this, but it's so big... need to cut this down)
        {0x9151fa00, 0x9151fa00+0x2A9A00, nullptr, "first half of Fighter1Resource"}, // first half of Fighter1Resource
        //{0x9151fa00 + 0x2A9A00, 0x91a72e00, nullptr, "second half of Fighter1Resource"}, // second half of Fighter1Resource

        //{0x91b04c80, 0x92058080, nullptr, "Fighter2Resource"}, // Fighter2Resource
        {0x91b04c80, 0x91b04c80+0x2A9A00, nullptr, "Fighter2Resource first half"}, // Fighter2Resource first half
        //{0x91b04c80+0x2A9A00, 0x92058080, nullptr, "Fighter2Resource second half"}, // Fighter2Resource second half
        





        //{0x920e9f00, 0x9263d300, nullptr, "Fighter3Resource"}, // Fighter3Resource
        //{0x926cf180, 0x92c22580, nullptr, "Fighter4Resource"}, // Fighter4Resource
        //{0x91a72e00, 0x91b04c80, nullptr, "Fighter1Resource2"}, // Fighter1Resource2
        //{0x92058080, 0x920e9f00, nullptr, "Fighter2Resource2"}, // Fighter2Resource2
        //{0x9263d300, 0x926cf180, nullptr, "Fighter3Resource2"}, // Fighter3Resource2
        //{0x92c22580, 0x92cb4400, nullptr, "Fighter4Resource2"}, // Fighter4Resource2
        {0x91478e00, 0x914d2900, nullptr, "FighterEffect"}, // FighterEffect
        {0x92cb4400, 0x92dcdf00, nullptr, "FighterTechqniq"}, // FighterTechqniq
        //{0x914d2900, 0x914ec400, nullptr, "FighterKirbyResource1"}, // FighterKirbyResource1
        //{0x914ec400, 0x91505f00, nullptr, "FighterKirbyResource2"}, // FighterKirbyResource2
        //{0x91505f00, 0x9151fa00, nullptr, "FighterKirbyResource3"}, // FighterKirbyResource3
        //{0x92dcdf00, 0x92e34600, nullptr, "AssistFigureResource"}, // AssistFigureResource
        //{0x9359ae00, 0x935ce200, nullptr, "ItemExtraResource"}, // ItemExtraResource
        //{0x92e34600, 0x92f34700, nullptr, "PokemonResource"}, // PokemonResource
        {0x9134cc00, 0x91478e00, nullptr, "CopyFB"}, // CopyFB
        {0x90167400, 0x90199800, nullptr, "GameGlobal"}, // GameGlobal
        //{0x90fddc00, 0x91018b00, nullptr, "GlobalMode"}, // GlobalMode
    };



    // wip
    static std::vector<PreserveBlock> excludeSections = {
        // {start address, size}

        {0x935d7660, 0x000089a0}, // CPP Framework heap (subject to change...??)

        //{0x805bacc0, 0x40 * 5}, // gfPadSystem
        
        {0x80663e00, 0x1a4}, // CameraController
        {0x80663b40, 0x198}, // cmAiController
        {0x805b6d20, 0x740}, // gfCameraManager
        
    };

    //SlippiInitBackupLocations(this->backupLocs, allMem, excludeSections);
    //SlippiInitBackupLocations(this->backupLocs, fullBackupRegions, excludeSections);
    SlippiInitBackupLocations(this->backupLocs, memRegions, excludeSections);
    //SlippiInitBackupLocations(this->backupLocs, test, excludeSections);
  
    static bool once = true;
    if (once) {
        u64 totalsize = 0;
        for (auto& loc : this->backupLocs) {
            u32 size = loc.endAddress-loc.startAddress;
            double newsize = ((double)size / 1000.0) / 1000.0;
            INFO_LOG_FMT(BRAWLBACK, "Savestate region: {:#x} - {:#x} : size {} mb   {}\n", loc.startAddress, loc.endAddress, newsize, loc.regionName);
            totalsize += size;
        }
        double dsize = ((double)totalsize / 1000.0) / 1000.0;
        std::string savestates_str = "Savestates total size: " + std::to_string(dsize) + " mb\n";
        OSD::AddTypedMessage(OSD::MessageType::Typeless, savestates_str, OSD::Duration::NORMAL, OSD::Color::GREEN);
        INFO_LOG_FMT(BRAWLBACK, "Savestates total size: {} mb\n", dsize);
    }
    once = false;
}

typedef std::vector<SlippiUtility::Savestate::ssBackupLoc>::iterator backupLocIterator;

void captureMemRegions(backupLocIterator start, backupLocIterator end, Core::System& system) {

    /* https://stackoverflow.com/questions/18314523/sse-copy-avx-copy-and-stdcopy-performance
    int GetMSB(u32* dwordPtr)
    {
        if(dwordPtr)
        {
            int result = 1;
    #if defined(_WIN64)
            if(dwordPtr & 0xFFFFFFFF00000000) { result += 32; dwordPtr &= 0xFFFFFFFF00000000; }
            if(dwordPtr & 0xFFFF0000FFFF0000) { result += 16; dwordPtr &= 0xFFFF0000FFFF0000; }
            if(dwordPtr & 0xFF00FF00FF00FF00) { result += 8;  dwordPtr &= 0xFF00FF00FF00FF00; }
            if(dwordPtr & 0xF0F0F0F0F0F0F0F0) { result += 4;  dwordPtr &= 0xF0F0F0F0F0F0F0F0; }
            if(dwordPtr & 0xCCCCCCCCCCCCCCCC) { result += 2;  dwordPtr &= 0xCCCCCCCCCCCCCCCC; }
            if(dwordPtr & 0xAAAAAAAAAAAAAAAA) { result += 1; }
    #else
            if(dwordPtr & 0xFFFF0000) { result += 16; dwordPtr &= 0xFFFF0000; }
            if(dwordPtr & 0xFF00FF00) { result += 8;  dwordPtr &= 0xFF00FF00; }
            if(dwordPtr & 0xF0F0F0F0) { result += 4;  dwordPtr &= 0xF0F0F0F0; }
            if(dwordPtr & 0xCCCCCCCC) { result += 2;  dwordPtr &= 0xCCCCCCCC; }
            if(dwordPtr & 0xAAAAAAAA) { result += 1; }
    #endif
            return result;
        }
        else
        {
            return 0;
        }
    }
    u32* processMask, systemMask;
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    SetProcessAffinityMask(GetCurrentProcess(), 1 << (GetMSB(processMask) - 1) );
    // Set Process Priority. you can use REALTIME_PRIORITY_CLASS.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    */

    // The process affinity mask is a bitmask where each set bit represents a core on
	// which this process is allowed to run
    //u32 all_threads_mask = 0b11111111111111111111111111111111;
    //Common::SetCurrentThreadAffinity(all_threads_mask);
    //INFO_LOG(BRAWLBACK, "Starting save %u", Common::Timer::GetTimeUs());
    for (auto it = start; it != end; ++it) {
        auto size = it->endAddress - it->startAddress;
        system.GetMemory().CopyFromEmu(it->data, it->startAddress, size);  // game -> emu
    }
}

void BrawlbackSavestate::Capture()
{
    //auto middle = backupLocs.begin() + backupLocs.size() / 2;
    captureMemRegions(backupLocs.begin(), backupLocs.end(), *m_system);

    //INFO_LOG(BRAWLBACK, "Starting save1 %u", Common::Timer::GetTimeUs());
    //std::thread capture1(captureMemRegions, backupLocs.begin(), middle);
    //INFO_LOG(BRAWLBACK, "Starting save2 %u", Common::Timer::GetTimeUs());
    //std::thread capture2(captureMemRegions, middle, backupLocs.end());


    //capture1.join();
    //capture2.join();
    /*
    // copy game mem
    for (auto it = backupLocs.begin(); it != backupLocs.end(); ++it)
    {
        auto size = it->endAddress - it->startAddress;
        // since no addresses will be lower than this, if this is the case, the backupLoc endAddress
        // represents a size rather than an end address
        //if (it->endAddress < LOW_BOUND_MEM) 
        //{
        //    Memory::CopyFromEmu(it->data, it->startAddress, it->endAddress);
        //}
        //else
        //{
            Memory::CopyFromEmu(it->data, it->startAddress, size);  // game -> emu
        //}
    }
    */

    // copy dolphin states
    //u8 *ptr = &dolphinSsBackup[0];
    //PointerWrap p(&ptr, PointerWrap::MODE_WRITE);
    //getDolphinState(p);

}

void BrawlbackSavestate::Load(std::vector<PreserveBlock> blocks)
{

    // Back up regions of game that should stay the same between savestates

    for (auto it = blocks.begin(); it != blocks.end(); ++it)
    {
        if (!preservationMap.count(*it)) // if this PreserveBlock is NOT in our preservationMap
        {
            // TODO: Clear preservation map when game ends
            preservationMap[*it] = std::vector<u8>(it->length); // init new entry at this PreserveBlock key
        }

        m_system->GetMemory().CopyFromEmu(&preservationMap[*it][0], it->address, it->length);
    }


    // Restore memory blocks
    for (auto it = backupLocs.begin(); it != backupLocs.end(); ++it)
    {
        auto size = it->endAddress - it->startAddress;
        //if (it->endAddress < LOW_BOUND_MEM)
        //{
        //    Memory::CopyToEmu(it->startAddress, it->data, it->endAddress);  // emu -> game
        //}
        //else
        //{
            m_system->GetMemory().CopyToEmu(it->startAddress, it->data, size);  // emu -> game
        //}
    }

    //// Restore audio
    //u8 *ptr = &dolphinSsBackup[0];
    //PointerWrap p(&ptr, PointerWrap::MODE_READ);
    //getDolphinState(p);

    // Restore preservation blocks
    for (auto it = blocks.begin(); it != blocks.end(); ++it)
    {
        m_system->GetMemory().CopyToEmu(it->address, &preservationMap[*it][0], it->length);
    }
  

}
