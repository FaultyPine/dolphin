#pragma once
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/System.h"
#include <string>
#include <vector>
#include <memory>
#include <deque>
#include "Core/Brawlback/Savestate.h"
#include "Core/Brawlback/BrawlbackUtility.h"
#include "Core/Brawlback/Netplay/Netplay.h"
#include "Core/Brawlback/Netplay/Matchmaking.h"
#include "Core/Brawlback/TimeSync.h"

using namespace Brawlback;

class CEXIBrawlback : public ExpansionInterface::IEXIDevice
{

public:
    explicit CEXIBrawlback(Core::System& system);
    ~CEXIBrawlback() override;

    
    void DMAWrite(u32 address, u32 size) override;
    void DMARead(u32 address, u32 size) override;

    bool IsPresent() const;

private:

    // byte vector for sending into to the game
    std::vector<u8> read_queue = {};


    // --- DMA handlers
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
    void NetplayThreadFunc();
    void ProcessNetReceive(ENetEvent* event);
    void ProcessRemoteFrameData(Match::PlayerFrameData* framedata, u8 numFramedatas);
    void ProcessIndividualRemoteFrameData(Match::PlayerFrameData* framedata);
    void ProcessGameSettings(Match::GameSettings* opponentGameSettings);
    void ProcessFrameAck(FrameAck* frameAck);
    u32 GetLatestRemoteFrame();
    ENetHost* server = nullptr;
    std::thread netplay_thread;
    std::unique_ptr<BrawlbackNetplay> netplay;

    bool isConnected = false;
    // -------------------------------



    // --- Matchmaking
    void connectToOpponent();
    void MatchmakingThreadFunc();
    Brawlback::UserInfo getUserInfo();
    Matchmaking::MatchSearchSettings lastSearch;
    std::unique_ptr<Matchmaking> matchmaking;
    std::thread matchmaking_thread;
    // -------------------------------




    // --- Game info
    bool isHost = true;
    int localPlayerIdx = -1;
    u8 numPlayers = 0;
    bool hasGameStarted = false;
    std::unique_ptr<Match::GameSettings> gameSettings;
    // -------------------------------


    // --- Time sync
    std::unique_ptr<TimeSync> timeSync;
    // -------------------------------

    
    // --- Rollback
    Match::RollbackInfo rollbackInfo = Match::RollbackInfo();
    void SetupRollback(u32 currentFrame, u32 confirmFrame);
    std::optional<Match::PlayerFrameData> HandleInputPrediction(u32 frame, u8 playerIdx);
    int latestConfirmedFrame = 0;
    // -------------------------------



    // --- Savestates
    std::deque<std::unique_ptr<BrawlbackSavestate>> savestates = {};
    std::unordered_map<u32, BrawlbackSavestate*> savestatesMap = {};

    std::map<s32, std::unique_ptr<BrawlbackSavestate>> activeSavestates = {};
	std::deque<std::unique_ptr<BrawlbackSavestate>> availableSavestates = {};
    // -------------------------------
    

    // --- Framedata (player inputs)
    void handleSendInputs(u32 frame);
    std::pair<bool, bool> getInputsForGame(Match::FrameData& framedataToSendToGame, u32 frame);
    void storeLocalInputs(Match::PlayerFrameData* localPlayerFramedata);

    // local player input history. Always holds FRAMEDATA_MAX_QUEUE_SIZE of past inputs
    PlayerFrameDataQueue localPlayerFrameData = {};

    // remote player input history (indexes are player indexes). Always holds FRAMEDATA_MAX_QUEUE_SIZE of past inputs
    std::array<PlayerFrameDataQueue, MAX_NUM_PLAYERS> remotePlayerFrameData = {};
    // -------------------------------



    protected:
    void TransferByte(u8& byte) override;

};
