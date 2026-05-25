#pragma once

#include <unordered_map>
#include <array>
#include <fstream>
#include <optional>

#include "Common/FileUtil.h"
#include "Common/CommonTypes.h"
#include "Common/Timer.h"
#include "Core/Brawlback/Brawltypes.h"

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"

#include "SlippiUtility.h"
#include "Savestate.h"

// make sure this is the same as the one in Brawlback.h on the game side
#define MAX_ROLLBACK_FRAMES 5

#define FRAME_DELAY 2
static_assert(FRAME_DELAY >= 1);
static_assert(FRAME_DELAY + MAX_ROLLBACK_FRAMES >= 6); // minimum frames of "compensation"

#define ROLLBACK_IMPL true

// number of max FrameData's to keep in the (remote) queue
#define FRAMEDATA_MAX_QUEUE_SIZE 15 
static_assert(FRAMEDATA_MAX_QUEUE_SIZE > MAX_ROLLBACK_FRAMES);
// update ping display every X frames
#define PING_DISPLAY_INTERVAL 30

// check clock desynchronization every X frames
#define ONLINE_LOCKSTEP_INTERVAL 30

#define GAME_START_FRAME 0
//#define GAME_FULL_START_FRAME 1
// before this frame we basically use delay-based netcode to ensure things are reasonably synced up before doing rollback stuff
#define GAME_FULL_START_FRAME 100

#define MAX_REMOTE_PLAYERS 3
#define MAX_NUM_PLAYERS 4
#define BRAWLBACK_PORT 7779

#define TIMESYNC_MAX_US_OFFSET 10000 // 60% of a frame

//#define SYNCLOG


#define MS_IN_FRAME (1000 / 60)
#define USEC_IN_FRAME (MS_IN_FRAME*1000)
#define MS_TO_FRAMES(ms) (ms * 60 / 1000)
#define FRAMES_TO_MS(frames) (1000 * frames / 60)

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

      CMD_GET_MATCH_STATE = 4,
      CMD_SET_MATCH_SELECTIONS = 6,

      CMD_TIMER_START = 7,
      CMD_TIMER_END = 8,
      CMD_UPDATE = 9,
      
      CMD_GET_ONLINE_STATUS = 10,
      CMD_CLEANUP_CONNECTION = 11,
      CMD_GET_NEW_SEED = 12,
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
        #pragma pack(push, 4)

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


        struct RollbackInfo {
            bool isUsingPredictedInputs;
            u32 beginFrame; // frame we realized we have no remote inputs
            u32 endFrame; // frame we received new remote inputs, and should now resim with those
            FrameData predictedInputs;

            bool pastFrameDataPopulated;
            FrameData pastFrameDatas[MAX_ROLLBACK_FRAMES];

            bool hasPreserveBlocks;
            //std::vector<SlippiUtility::Savestate::PreserveBlock> preserveBlocks;

            RollbackInfo() {
                Reset();
            }
            void Reset() {
                isUsingPredictedInputs = false;
                beginFrame = 0;
                endFrame = 0;
                memset(&predictedInputs, 0, sizeof(FrameData));
                pastFrameDataPopulated = false;
                memset(pastFrameDatas, 0, sizeof(FrameData) * MAX_ROLLBACK_FRAMES);
                hasPreserveBlocks = false;
                //preserveBlocks = {};
            }

        };

        #pragma pack(pop)


        enum PlayerType : u8
        {
            PLAYERTYPE_LOCAL = 0x0,
            PLAYERTYPE_REMOTE = 0x1,
        };

        
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

        bool isPlayerFrameDataEqual(const PlayerFrameData& p1, const PlayerFrameData& p2);
        
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
        std::string stringifyFramedata(const Match::FrameData& fd, int numPlayers);
        std::string stringifyFramedata(const Match::PlayerFrameData& pfd);
    }
    
    typedef std::deque<std::unique_ptr<Match::PlayerFrameData>> PlayerFrameDataQueue;

    Match::PlayerFrameData* findInPlayerFrameDataQueue(const PlayerFrameDataQueue& queue, u32 frame);

    int SavestateChecksum(std::vector<ssBackupLoc>* backupLocs);

    template <typename T>
    T Clamp(T input, T Max, T Min) {
        return input > Max ? Max : ( input < Min ? Min : input );
    }


    inline int MAX(int x, int y) { return (((x) > (y)) ? (x) : (y)); }
    inline int MIN(int x, int y) { return (((x) < (y)) ? (x) : (y)); }
    // 1 if in range (inclusive), 0 otherwise
    inline int RANGE(int i, int min, int max) { return ((i < min) || (i > max) ? 0 : 1); }

    namespace Dump {
        void DoMemDumpIteration(int& dump_num);
        void DumpMem(AddressSpace::Type memType, const std::string& dumpPath);
    }

}
