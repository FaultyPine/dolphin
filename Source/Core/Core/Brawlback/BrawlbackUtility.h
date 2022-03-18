#pragma once

#include <unordered_map>
#include <array>
#include <fstream>

#include "Common/FileUtil.h"
#include "Common/CommonTypes.h"
#include "Common/Timer.h"
#include "Core/Brawlback/Brawltypes.h"
#include "Core/Brawlback/Savestate.h"

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"

#include "SlippiUtility.h"

#define MAX_ROLLBACK_FRAMES 7

// must be >= 1
#define FRAME_DELAY 3

#define ROLLBACK_IMPL false

// number of max FrameData's to keep in the queue
#define FRAMEDATA_MAX_QUEUE_SIZE 120 
// update ping display every X frames
#define PING_DISPLAY_INTERVAL 60

#define ONLINE_LOCKSTEP_INTERVAL 30
#define GAME_START_FRAME 0
//#define GAME_FULL_START_FRAME 1
#define GAME_FULL_START_FRAME 250

#define MAX_REMOTE_PLAYERS 3
#define MAX_NUM_PLAYERS 4
#define BRAWLBACK_PORT 7779

// GGPO
#define GGPO_LOG_ENABLE true
#define GGPO_LOG_TIMESTAMPS false
#define GGPO_OOP_PERCENT 0
// ------------


// 59.94 Hz (16.66 ms in a frame for 60fps)  ( -- is this accurate? This is the case for melee, idk if it also applies here)
//#define USEC_IN_FRAME 16683
#define USEC_IN_FRAME 16666

// ---
// mem dumping related
#include "Core/HW/AddressSpace.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
// ---

namespace Brawlback {
    const u8 NAMETAG_SIZE = 8;
    const u8 DISPLAY_NAME_SIZE = 31;
    const u8 CONNECT_CODE_SIZE = 10;

    struct UserInfo
    {
      std::string uid = "";
      std::string playKey = "";
      std::string displayName = "";
      std::string connectCode = "";
      std::string latestVersion = "";
      std::string fileContents = "";

      int port;
    };

    enum EXICommand : u8
    {
      CMD_UNKNOWN = 0,


      CMD_ONLINE_INPUTS = 1,
      CMD_CAPTURE_SAVESTATE = 2,
      CMD_LOAD_SAVESTATE = 3,

      CMD_FIND_OPPONENT = 5,
      CMD_START_MATCH = 13,
      CMD_SETUP_PLAYERS = 14,
      CMD_FRAMEDATA = 15,
      CMD_TIMESYNC = 16,
      CMD_ROLLBACK = 17,

      CMD_START_TIMER = 7,
      CMD_END_TIMER = 8,

      CMD_GAME_PROC_OVERRIDE = 9,
      CMD_GAME_PROC = 10,

    };

    enum NetPacketCommand : u8 
    {
        CMD_FRAME_DATA = 1,
        CMD_GAME_SETTINGS = 2,
        CMD_FRAME_DATA_ACK = 3,
    };

    struct FrameOffsetData {
        int idx;
        std::vector<s32> buf;
    };

    namespace Match
    {

        enum PlayerType : u8
        {
            PLAYERTYPE_LOCAL = 0x0,
            PLAYERTYPE_REMOTE = 0x1,
        };

        
        struct PlayerFrameData {
            u32 frame;
            u8 playerIdx;
            BrawlbackPad pad;

            // do these impact the size of the struct?
            // wouldn't the vtable ptr screw with it being interpreted on gameside???
            // (since the gameside structs don't have these ctors)
            PlayerFrameData() {
                frame = 0;
                playerIdx = 0;
                pad = BrawlbackPad();
            }
            PlayerFrameData(u32 _frame, u8 _playerIdx) {
                frame = _frame;
                playerIdx = _playerIdx;
                pad = BrawlbackPad();
            }
        };

        //#pragma pack(push, 4)
        struct FrameData {
            u32 randomSeed;
            PlayerFrameData playerFrameDatas[MAX_NUM_PLAYERS];

            FrameData() {
                randomSeed = 0;
                for (int i = 0; i < MAX_NUM_PLAYERS; i++) {
                    playerFrameDatas[i] = PlayerFrameData();
                }
            }
            FrameData(u32 frame) {
                randomSeed = 0;
                for (u8 i = 0; i < MAX_NUM_PLAYERS; i++) {
                    playerFrameDatas[i] = PlayerFrameData(frame, i);
                }
            }
        };
        //#pragma pack(pop)

        struct PlayerSettings
        {
            u8 charID;
            u8 charColor;
            PlayerType playerType;
            u8 controllerPort;
            u16 nametag[NAMETAG_SIZE];
            u8 displayName[DISPLAY_NAME_SIZE];
            u8 connectCode[CONNECT_CODE_SIZE];
        };

        struct GameSettings
        {
            u8 localPlayerIdx;
            u8 numPlayers;
            u16 stageID;
            u32 randomSeed;
            PlayerSettings playerSettings[MAX_NUM_PLAYERS];
        };

        struct Game {
            u32 version;
            GameSettings gameSettings;
            u32 currentFrame;
            std::unordered_map<int32_t, FrameData*> framesByIndex;
            std::vector<std::unique_ptr<FrameData>> frames;
        };

        struct RollbackInfo {
            bool isUsingPredictedInputs;
            u32 beginFrame; // frame we realized we have no remote inputs
            u32 endFrame; // frame we received new remote inputs, and should now resim with those
            FrameData predictedInputs;

            bool pastFrameDataPopulated;
            FrameData pastFrameDatas[MAX_ROLLBACK_FRAMES];

            bool hasPreserveBlocks;
            std::vector<SlippiUtility::Savestate::PreserveBlock> preserveBlocks;

            RollbackInfo() {
                Reset();
            }
            void Reset() {
                isUsingPredictedInputs = false;
                beginFrame = 0;
                endFrame = 0;
                predictedInputs = FrameData();
                pastFrameDataPopulated = false;
                memset(pastFrameDatas, 0, sizeof(FrameData) * MAX_ROLLBACK_FRAMES);
                hasPreserveBlocks = false;
                preserveBlocks = {};
            }

        };
        
    }




    // checks if the specified `button` is held down based on the buttonBits bitfield
    bool isButtonPressed(u16 buttonBits, PADButtonBits button);

    namespace Mem {
        void print_byte(uint8_t byte);
        void print_half(u16 half);
        void print_word(u32 word);

        template <typename T>
        std::vector<u8> structToByteVector(T* s) {
            auto ptr = reinterpret_cast<u8*>(s);
            return std::vector<u8>(ptr, ptr + sizeof(T));
        }

        void fillByteVectorWithBuffer(std::vector<u8>& vec, u8* buf, size_t size);
    }
    namespace Sync {
        std::string getSyncLogFilePath();
        std::string str_byte(uint8_t byte);
        std::string str_half(u16 half);
        void SyncLog(const std::string& msg);
        std::string stringifyFramedata(const Match::PlayerFrameData& pfd);
    }
    
    typedef std::deque<std::unique_ptr<Match::PlayerFrameData>> PlayerFrameDataQueue;
    typedef std::map<s32, std::unique_ptr<BrawlbackSavestate>> SavestateMap;
    typedef std::deque<std::unique_ptr<BrawlbackSavestate>> SavestateQueue;

    Match::PlayerFrameData* findInPlayerFrameDataQueue(const PlayerFrameDataQueue& queue, u32 frame);

    template <typename T>
    T Clamp(T input, T Max, T Min) {
        return input > Max ? Max : ( input < Min ? Min : input );
    }

    namespace Dump {
        void DoMemDumpIteration(int& dump_num);
        void DumpMem(AddressSpace::Type memType, const std::string& dumpPath);
    }

}
