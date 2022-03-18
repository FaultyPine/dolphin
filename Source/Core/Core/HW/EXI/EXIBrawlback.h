#pragma once
#include "Core/HW/EXI/EXI_Device.h"
#include <string>
#include <vector>
#include <memory>
#include <deque>
#include "Core/Brawlback/BrawlbackUtility.h"
#include "Core/Brawlback/Netplay/Netplay.h"
#include "Core/Brawlback/Netplay/Matchmaking.h"
#include "Core/Brawlback/TimeSync.h"
#include "Core/Brawlback/ggpo/ggpo_main.h"


using namespace Brawlback;

class CEXIBrawlback : public ExpansionInterface::IEXIDevice
{

public:
    CEXIBrawlback();
    ~CEXIBrawlback() override;

    
    void DMAWrite(u32 address, u32 size) override;
    void DMARead(u32 address, u32 size) override;

    bool IsPresent() const;
    
    static SavestateMap* getActiveSavestates();
    static SavestateQueue* getAvailableSavestates();
    static s32 getCurrentFrame();

    static SavestateMap activeSavestates;
	static SavestateQueue availableSavestates;
    static s32 currentFrame;

private:

    // byte vector for sending into to the game
    std::vector<u8> read_queue = {};


    // DMA handlers
    void handleCaptureSavestate(u8* data);
    void handleLoadSavestate(u8* data);
    void handleLocalPadData(u8* data);
    void handleFindMatch(u8* payload);
    void handleStartMatch(u8* payload);

    template <typename T>
    void SendCmdToGame(EXICommand cmd, T* payload);

    void SendCmdToGame(EXICommand cmd);
    // -------------------------------


    // --- Net
    void MatchmakingThreadFunc();
    void NetplayThreadFunc();
    void connectToOpponent();
    void ProcessNetReceive(ENetEvent* event);
    void ProcessRemoteFrameData(Match::PlayerFrameData* framedata, u8 numFramedatas);
    void ProcessIndividualRemoteFrameData(Match::PlayerFrameData* framedata);
    void ProcessGameSettings(Match::GameSettings* opponentGameSettings);
    void ProcessFrameAck(FrameAck* frameAck);
    u32 GetLatestRemoteFrame();
    ENetHost* server = nullptr;
    Matchmaking::MatchSearchSettings lastSearch;
    std::thread netplay_thread;
    std::thread matchmaking_thread;
    std::unique_ptr<BrawlbackNetplay> netplay;
    std::unique_ptr<Matchmaking> matchmaking;

    bool isConnected = false;
    // -------------------------------




    // --- Game info
    bool isHost = true;
    int localPlayerIdx = -1;
    u8 numPlayers = -1;
    bool hasGameStarted = false;
    std::unique_ptr<Match::GameSettings> gameSettings;
    // -------------------------------

    Brawlback::UserInfo getUserInfo();

    // --- Time sync
    void DropAckedInputs(u32 currFrame);
    std::unique_ptr<TimeSync> timeSync;
    // -------------------------------

    
    // --- Rollback
    Match::RollbackInfo rollbackInfo = Match::RollbackInfo();
    void SetupRollback(u32 frame);
    void HandleLocalInputsDuringPrediction(u32 frame, u8 playerIdx);
    // -------------------------------



    // --- Savestates

    //std::deque<std::unique_ptr<BrawlbackSavestate>> savestates = {};
    //std::unordered_map<u32, BrawlbackSavestate*> savestatesMap = {};

    
    // -------------------------------
    

    // --- Framedata (player inputs)
    void handleSendInputs(u32 frame);
    std::pair<bool, bool> getInputsForGame(Match::FrameData& framedataToSendToGame, u32 frame);
    void storeLocalInputs(Match::PlayerFrameData* localPlayerFramedata);

    // local player input history
    PlayerFrameDataQueue localPlayerFrameData = {};

    //std::unordered_map<u32, Match::PlayerFrameData*> localPlayerFrameDataMap = {};

    // remote player input history (indexes are player indexes)
    std::array<PlayerFrameDataQueue, MAX_NUM_PLAYERS> remotePlayerFrameData = {};
    // array of players - key is current frame, val is ptr to that frame's (player)framedata
    std::array<std::unordered_map<u32, Match::PlayerFrameData*>, MAX_NUM_PLAYERS> remotePlayerFrameDataMap = {};
    // -------------------------------


    // --- GGPO

    void handleGameProcOverride(u8* payload);
    void RunFrame(Match::PlayerFrameData* localInputs);
    // -------------------------------


    protected:
    void TransferByte(u8& byte) override;

};
