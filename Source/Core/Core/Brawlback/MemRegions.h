#pragma once


#include "SlippiUtility.h"


static std::vector<ssBackupLoc> memRegions = {
        // mem1
        {0x805b8a00, 0x805b8a00+0x17c, nullptr, "gfTaskScheduler"}, // gfTaskScheduler 
        
        {0x80611f60, 0x80673460, nullptr, "System"}, // System
        {0x80b8db60, 0x80c23a60, nullptr, "Effect"}, // Effect
        {0x8123ab60, 0x8128cb60, nullptr, "Fighter1Instance"}, // Fighter1Instance
        {0x8128cb60, 0x812deb60, nullptr, "Fighter2Instance"}, // Fighter2Instance
        {0x81601960, 0x81734d60, nullptr, "InfoInstance"}, // InfoInstance
        {0x81734d60, 0x817ce860, nullptr, "MenuInstance"}, // MenuInstance

        //{0x80A471A0, 0x80b8db60, nullptr, "OverlayCommon 4/4"}, // OverlayCommon 4/4
        //{0x80A471A0, 0x80A471A0+0xA34E0, nullptr, "OverlayCommon 7/8"},
        {0x80A471A0+0xA34E0, 0x80b8db60, nullptr, "OverlayCommon 8/8"},

        // mem2
        {0x90e61400, 0x90e77500, nullptr, "WiiPad"}, // WiiPad

        //{0x9151fa00, 0x917C9400, nullptr, "first half of Fighter1Resource"}, // first half of Fighter1Resource
        {0x9151fa00, 0x9151fa00+0x154D00, nullptr, "fighter1resource 1/4"},
        //{0x9151fa00+0x154D00, 0x917C9400, nullptr, "fighter1resource 2/4"},


        //{0x91b04c80, 0x91DAE680, nullptr, "Fighter2Resource first half"}, // Fighter2Resource first half
        {0x91b04c80, 0x91b04c80+0x154D00, nullptr, "fighter2resource 1/4"},
        //{0x91b04c80+0x154D00, 0x91DAE680, nullptr, "fighter2resource 2/4"},

        {0x91478e00, 0x914d2900, nullptr, "FighterEffect"}, // FighterEffect


        //{0x92cb4400, 0x92dcdf00, nullptr, "FighterTechqniq"}, // FighterTechqniq
        //{0x92cb4400, 0x92cb4400+0x8CD80, nullptr, "FighterTechqniq 1/2"},
        {0x92cb4400+0x8CD80, 0x92dcdf00, nullptr, "fightertechqniq 2/2"},


        //{0x9134cc00, 0x91478e00, nullptr, "CopyFB"}, // CopyFB

        //{0x9134cc00, 0x913E2D00, nullptr, "CopyFB 1/2"},
        //{0x913E2D00, 0x91478e00, nullptr, "CopyFB 2/2"},

        {0x9134cc00, 0x9134cc00+0x4B080, nullptr, "CopyFB 1/4"},
        //{0x9134cc00+0x4B080, 0x913E2D00, nullptr, "CopyFB 2/4"},



        {0x90167400, 0x90199800, nullptr, "GameGlobal"}, // GameGlobal

};