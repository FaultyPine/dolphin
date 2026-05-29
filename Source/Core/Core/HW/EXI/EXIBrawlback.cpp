

#include "EXIBrawlback.h"
#include "Core/ConfigManager.h"

#include "Core/HW/Memmap.h"
#include "Core/Rollback/RollbackManager.h"
#include <chrono>
#include <iostream>
#include "VideoCommon/OnScreenDisplay.h"


// --- Mutexes
std::mutex read_queue_mutex = std::mutex();
std::mutex remotePadQueueMutex = std::mutex();
std::mutex localPadQueueMutex = std::mutex();
// -------------------------------

CEXIBrawlback::CEXIBrawlback(Core::System& system) : IEXIDevice(system)
{
    INFO_LOG_FMT(BRAWLBACK, "BRAWLBACK EXI device created");
#ifdef _WIN32
    if (std::filesystem::exists(Sync::getSyncLogFilePath())) {
        std::filesystem::remove(Sync::getSyncLogFilePath());
    }
#endif

    INFO_LOG_FMT(BRAWLBACK, "BRAWLBACK exi ctor");
    // TODO: initialize this only when finding matches
    auto enet_init_res = enet_initialize();
    if (enet_init_res < 0) {
        ERROR_LOG_FMT(BRAWLBACK, "Failed to init enet! {}\n", enet_init_res);
    }
    else if (enet_init_res == 0) {
        INFO_LOG_FMT(BRAWLBACK, "Enet init success");
    }
    this->netplay = std::make_unique<BrawlbackNetplay>();
    this->matchmaking = std::make_unique<Matchmaking>(this->getUserInfo());
    this->timeSync = std::make_unique<TimeSync>();
}

Brawlback::UserInfo CEXIBrawlback::getUserInfo() {
  Brawlback::UserInfo info;

  std::string lylat;
#ifdef _WIN32
    std::string lylat_json_path = File::GetExeDirectory() + "/lylat.json";
#else
    // This will look on "~/Libraries/Application Support/Dolphin/lylat.json" on macosx
    std::string lylat_json_path = File::GetUserPath(D_USER_IDX) + "lylat.json";
#endif
    INFO_LOG_FMT(BRAWLBACK, "Reading lylat json from {}\n", lylat_json_path);
    std::string data;
    if (!File::ReadFileToString(lylat_json_path, data))
    {
        ERROR_LOG_FMT(BRAWLBACK, "Could not find lylat.json.");
        return info;
    }

  json j = json::parse(data);
  INFO_LOG_FMT(BRAWLBACK, "JSON Contents: {}", j.dump(4));

  info.uid = j["uid"].get<std::string>();
  info.playKey = j["playKey"].get<std::string>();
  info.connectCode = j["connectCode"].get<std::string>();
  info.displayName = "Dev Test";
  info.latestVersion = "test";
  info.fileContents = "test";

  return info;
}

CEXIBrawlback::~CEXIBrawlback()
{
    enet_deinitialize();
    enet_host_destroy(this->server);
    this->isConnected = false;
    if (this->netplay_thread.joinable()) {
        this->netplay_thread.join();
    }

    delete this->matchmaking.release();
    if (this->matchmaking_thread.joinable()) {
      this->matchmaking_thread.join();
    }
}



void CEXIBrawlback::handleCaptureSavestate(u8* data)
{
    u64 startTime = Common::Timer::NowUs();

    int idx = 0;
    s32 frame = (s32)SlippiUtility::Mem::readWord(data, idx, 999, 0);

    auto& rbMgr = Rollback::RollbackManager::Get();

    if (frame == GAME_START_FRAME)
    {
        // Re-init the ring buffer at game start so stale slots from any previous
        // session never leak into this game's rollback window.
        rbMgr.Init(m_system);
        m_lastCapturedFrame = -1;
        INFO_LOG_FMT(BRAWLBACK, "RollbackManager re-initialized for new game\n");
    }

    rbMgr.SaveFrame(m_system);
    m_lastCapturedFrame = (int)frame;

    u32 timeDiff = (u32)(Common::Timer::NowUs() - startTime);
    INFO_LOG_FMT(BRAWLBACK, "Captured savestate for frame {} in: {} ms", frame, ((double)timeDiff) / 1000);
}

void CEXIBrawlback::handleLoadSavestate(u8* data)
{
    Match::RollbackInfo* loadStateRollbackInfo = (Match::RollbackInfo*)data;
    // The frame we should restore is the one we first began not receiving inputs.
    s32 frame = (s32)SlippiUtility::Mem::readWord((u8*)&loadStateRollbackInfo->beginFrame);

    auto& rbMgr = Rollback::RollbackManager::Get();

    // `frames_back` = how many SaveFrame calls separate `frame` from the most-recently
    // captured slot.  Must be in [1, MAX_ROLLBACK_FRAMES].
    const int frames_back = m_lastCapturedFrame - (int)frame;

    if (!rbMgr.IsInitialized() || frames_back < 1 || frames_back > MAX_ROLLBACK_FRAMES)
    {
        ERROR_LOG_FMT(BRAWLBACK,
                      "Cannot load savestate for frame {} (lastCaptured={}, frames_back={}).",
                      frame, m_lastCapturedFrame, frames_back);
        PanicAlertFmtT("Savestate for frame {0} does not exist.", frame);
        this->isConnected = false;
        if (this->server)
        {
            for (size_t i = 0; i < this->server->peerCount; i++)
                enet_peer_disconnect(&this->server->peers[i], 0);
        }
        return;
    }

    u64 startTime = Common::Timer::NowUs();
    rbMgr.LoadFrame(m_system, frames_back);
    // After the load the ring is rewound; our "last captured" frame is now `frame`.
    m_lastCapturedFrame = (int)frame;

    u32 timeDiff = (u32)(Common::Timer::NowUs() - startTime);
    ERROR_LOG_FMT(BRAWLBACK, "Loaded savestate for frame {} (frames_back={}) in: {} ms",
                  frame, frames_back, ((double)timeDiff) / 1000);
}

template <typename T>
void CEXIBrawlback::SendCmdToGame(EXICommand cmd, T* payload) {
    //std::lock_guard<std::mutex> lock (read_queue_mutex); // crash here wtf?
    this->read_queue.clear();
    this->read_queue.push_back(cmd);
    if (payload) {
        std::vector<u8> byteVec = Mem::structToByteVector(payload);
        this->read_queue.insert(this->read_queue.end(), byteVec.begin(), byteVec.end());
    }
}

void CEXIBrawlback::SendCmdToGame(EXICommand cmd) {
    //std::lock_guard<std::mutex> lock (read_queue_mutex);
    this->read_queue.clear();
    this->read_queue.push_back(cmd);
}

Match::PlayerFrameData CreateDummyPlayerFrameData(u32 frame, u8 playerIdx) {
    Match::PlayerFrameData dummy_framedata = Match::PlayerFrameData();
    dummy_framedata.frame = frame;
    dummy_framedata.playerIdx = playerIdx;
    dummy_framedata.pad = BrawlbackPad(); // empty pad
    return dummy_framedata;
}

// `data` is a ptr to a PlayerFrameData struct
// this is called every frame at the beginning of the frame
void CEXIBrawlback::handleLocalPadData(u8* data)
{
    Match::PlayerFrameData* playerFramedata = (Match::PlayerFrameData*)data;
    int idx = 0;
    // first 4 bytes are current game frame
    u32 frame = SlippiUtility::Mem::readWord(data, idx, 999, 0); // properly switched endianness
    u8 playerIdx = playerFramedata->playerIdx;
    playerFramedata->frame = frame; // properly switched endianness

    if (frame == GAME_START_FRAME) {
        // push framedatas for first few delay frames
        for (int i = GAME_START_FRAME; i < FRAME_DELAY; i++) {
            this->remotePlayerFrameData[playerIdx].push_back(std::make_unique<Match::PlayerFrameData>(CreateDummyPlayerFrameData(i, playerIdx)));
            this->localPlayerFrameData.push_back(std::make_unique<Match::PlayerFrameData>(CreateDummyPlayerFrameData(i, playerIdx)));
        }
        this->hasGameStarted = true;
    }

    // this just for debugging purposes. Tracks and displays the number of timesyncs done every 60 frames
    static int numTimesyncs = 0;
    if (frame % 60 == 0) {
        OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer, "Timesyncs: " + std::to_string(numTimesyncs) + "\n", OSD::Duration::NORMAL, OSD::Color::CYAN);
        numTimesyncs = 0;
    }

    if (frame % PING_DISPLAY_INTERVAL == 0)
        OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer, "Time offset: " + std::to_string( (double)timeSync->calcTimeOffsetUs(this->numPlayers) / 1000 ) + " ms\n");
    
    int remote_frame = (int)this->GetLatestRemoteFrame();
    bool shouldTimeSync = this->timeSync->shouldStallFrame(frame, remote_frame, this->numPlayers);
    if (shouldTimeSync) {
        INFO_LOG_FMT(BRAWLBACK, "Should time sync\n");
        // Send inputs that have not yet been acked
        this->handleSendInputs(frame);
        this->SendCmdToGame(EXICommand::CMD_TIMESYNC);
        numTimesyncs += 1;
    }
    else {

        // store these local inputs (with frame delay)
        this->storeLocalInputs(playerFramedata);
        // broadcasts local inputs
        this->handleSendInputs(frame);

        // getting inputs for all players & sending them to game
        Match::FrameData framedataToSendToGame = Match::FrameData(frame);
        framedataToSendToGame.randomSeed = 0x496ffd00; // tmp
        // populates playerFrameDatas field of the above FrameData
        std::pair<bool, bool> foundData = this->getInputsForGame(framedataToSendToGame, frame);

        #if ROLLBACK_IMPL
        if (this->rollbackInfo.pastFrameDataPopulated) {
            this->SendCmdToGame(EXICommand::CMD_ROLLBACK, &this->rollbackInfo);
            this->rollbackInfo.Reset(); // reset rollbackInfo
            return;
        }
        #endif

        if (!foundData.second && frame > FRAME_DELAY) { // if we didn't find remote inputs AND didn't find/use predicted inputs
            ERROR_LOG_FMT(BRAWLBACK, "Couldn't find any remote inputs - Sending time sync command\n");
            this->SendCmdToGame(EXICommand::CMD_TIMESYNC);
            numTimesyncs += 1;
        }
        else {
            #ifdef SYNCLOG
            if (!rollbackInfo.isUsingPredictedInputs) {
                Sync::SyncLog(Sync::stringifyFramedata(framedataToSendToGame, 2));
            }
            #endif
            this->SendCmdToGame(EXICommand::CMD_FRAMEDATA, &framedataToSendToGame);
        }

    }
}

void CEXIBrawlback::storeLocalInputs(Match::PlayerFrameData* localPlayerFramedata) {
    std::lock_guard<std::mutex> local_lock (localPadQueueMutex);
    std::unique_ptr<Match::PlayerFrameData> pFD = std::make_unique<Match::PlayerFrameData>(*localPlayerFramedata);
    // local inputs offset by FRAME_DELAY to mask latency
    // Once we hit frame X, we send inputs for that frame, but pretend they're from frame X+2
    // so those inputs now have an extra 2 frames to get to the opponent before the opponent's
    // client hits frame X+2.
    pFD->frame += FRAME_DELAY;
    //INFO_LOG(BRAWLBACK, "Frame %u PlayerIdx: %u numPlayers %u\n", localPlayerFramedata->frame, localPlayerFramedata->playerIdx, this->numPlayers);

    // make sure we're storing inputs sequentially
    if (!this->localPlayerFrameData.empty() && pFD->frame != this->localPlayerFrameData.back()->frame + 1) {
        WARN_LOG_FMT(BRAWLBACK, "Didn't push local framedata for frame {}\n", pFD->frame);
    }
    else {
        // store local framedata
        if (this->localPlayerFrameData.size() + 1 > FRAMEDATA_MAX_QUEUE_SIZE) {
            //INFO_LOG(BRAWLBACK, "Popping local framedata for frame %u\n", this->localPlayerFrameData.front()->frame);
            this->localPlayerFrameData.pop_front();
        }
        this->localPlayerFrameData.push_back(std::move(pFD));
    }
}

void CEXIBrawlback::handleSendInputs(u32 frame) {
    // broadcast this local framedata
    if (!this->localPlayerFrameData.empty()) {
        std::lock_guard<std::mutex> local_lock (localPadQueueMutex);
        
        // each frame we send local inputs to the other client(s)
        // those clients then acknowledge those inputs and send that ack(nowledgement)
        // back to us. All acked inputs are irrelevant (unless we need to rollback)
        // since we know for a fact the remote client has them.
        // We track what frame of inputs has been acked ("localHeadFrame")
        // so that when we go to send inputs, we only send the un-acked ones.
        // We send *all* unacked inputs so that when the remote client doesn't receive inputs, and needs to rollback
        // the next packet will have all the inputs that that client hasn't received.
        int minAckFrame = this->timeSync->getMinAckFrame(this->numPlayers);

        // clamp to current frame to prevent it dropping local inputs that haven't been used yet
        minAckFrame = MIN(minAckFrame, frame); 


        int localPadQueueSize = (int)this->localPlayerFrameData.size();
        if (localPadQueueSize == 0) return; // no inputs, nothing to send
        int endIdx = localPadQueueSize-1 - (this->localPlayerFrameData.back()->frame - minAckFrame);

        std::vector<Match::PlayerFrameData*> localFramedatas = {};
        // push framedatas from back to front
        // this means the resulting vector (localFramedatas) will have the most
        // recent framedata in the first position, and the oldest framedata in the last position
        for (int i = localPadQueueSize-1; i > endIdx; i--) {
            const auto& localFramedata = this->localPlayerFrameData[i];
            // make sure we queue up these inputs sequentially
            if (localFramedatas.empty() || ( !localFramedatas.empty() && localFramedatas.back()->frame > localFramedata->frame) ) {
                Match::PlayerFrameData* inputToSend = localFramedata.get();
                localFramedatas.push_back(inputToSend);
            }
        }

        //INFO_LOG(BRAWLBACK, "Broadcasting %i framedatas\n", localFramedatas.size());
        this->netplay->BroadcastPlayerFrameDataWithPastFrames(this->server, localFramedatas);

        u32 mostRecentFrame = this->localPlayerFrameData.back()->frame; // current frame with delay
        this->timeSync->TimeSyncUpdate(mostRecentFrame, this->numPlayers);

    }
}

std::pair<bool, bool> CEXIBrawlback::getInputsForGame(Match::FrameData& framedataToSendToGame, u32 frame) {
    std::lock_guard<std::mutex> lock (remotePadQueueMutex);

    // first is if we've found local inputs, second is if we've found remote inputs
    std::pair<bool, bool> foundData = std::make_pair(false, false);

    // for each player
    for (int playerIdx = 0; playerIdx < this->numPlayers; playerIdx++) {
        // --------- search for local player's inputs -------------
        if (playerIdx == this->localPlayerIdx && !this->localPlayerFrameData.empty()) {
            std::lock_guard<std::mutex> local_lock (localPadQueueMutex);

            Match::PlayerFrameData* localFrameData = findInPlayerFrameDataQueue(this->localPlayerFrameData, frame);
            foundData.first = localFrameData != nullptr;
            if (localFrameData) {
                framedataToSendToGame.playerFrameDatas[this->localPlayerIdx] = *localFrameData;
            }

            if (!foundData.first) {
                // this shouldn't ever happen lol. Just putting this here so things don't go totally haywire
                ERROR_LOG_FMT(BRAWLBACK, "Couldn't find local inputs! Using empty pad.\n");
                WARN_LOG_FMT(BRAWLBACK, "Local pad input range: [{} - {}]\n", this->localPlayerFrameData.back()->frame, this->localPlayerFrameData.front()->frame);
                framedataToSendToGame.playerFrameDatas[this->localPlayerIdx] = CreateDummyPlayerFrameData(frame, this->localPlayerIdx);
            }
            continue;

        }
        // ------------------------------------------

        // -------- search for remote player's inputs --------
        if (!this->remotePlayerFrameData.empty() && !this->remotePlayerFrameData[playerIdx].empty()) {
            // find framedata in queue that has the frame we want to inject into the game (current frame - frame delay)
            //INFO_LOG(BRAWLBACK, "Remote framedata q range: %u - %u\n", this->remotePlayerFrameData[playerIdx].front()->frame, this->remotePlayerFrameData[playerIdx].back()->frame);
            
            Match::PlayerFrameData* remoteFrameData = findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], frame);
            foundData.second = remoteFrameData != nullptr;
            if (remoteFrameData) {
                framedataToSendToGame.playerFrameDatas[playerIdx] = *remoteFrameData;
                // we've just received this frame's inputs, so if we were using predicted inputs, we now need to rollback
                #if ROLLBACK_IMPL
                if ( ( this->rollbackInfo.isUsingPredictedInputs || this->latestConfirmedFrame < (int)frame ) && frame > GAME_FULL_START_FRAME) {
                    this->SetupRollback(frame, frame);
                }
                #else
                if (false) {}
                #endif
                else {
                    INFO_LOG_FMT(BRAWLBACK, "found remote inputs frame {}\n", frame);
                    this->latestConfirmedFrame = (int)frame;
                }
            }

        }
        // --------------------------------------------------

        #if ROLLBACK_IMPL
        if (!foundData.second && frame > GAME_FULL_START_FRAME) { // didn't find framedata for this frame.
            INFO_LOG_FMT(BRAWLBACK, "no remote framedata - frame {} remotePIdx {}\n", frame, playerIdx);
            if (this->remotePlayerFrameData[playerIdx].size() >= MAX_ROLLBACK_FRAMES) {

                Match::PlayerFrameData* justRecvdInputs = nullptr;
                for (s32 i = (s32)frame; i >= latestConfirmedFrame+1 && !justRecvdInputs; i--) {
                    justRecvdInputs = findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], i);
                }

                if (justRecvdInputs && latestConfirmedFrame >= GAME_FULL_START_FRAME) {
                    INFO_LOG_FMT(BRAWLBACK, "Found past remote inputs for rollback. justRecvdInputs->frame = {} rbBeginFrame = {}", justRecvdInputs->frame, rollbackInfo.beginFrame);
                    this->SetupRollback(frame, justRecvdInputs->frame);
                }
                else {
                    if (!this->rollbackInfo.isUsingPredictedInputs) {
                        std::optional<Match::PlayerFrameData> predictedInputs = this->HandleInputPrediction(frame, playerIdx);
                        if (predictedInputs) {
                            foundData.second = true;
                            // rekey predicted inputs for this frame
                            predictedInputs.value().frame = frame;
                            framedataToSendToGame.playerFrameDatas[playerIdx] = predictedInputs.value();
                        }
                    }
                    else {
                        Match::PlayerFrameData predictedInputs = this->rollbackInfo.predictedInputs.playerFrameDatas[playerIdx];
                        if (frame - predictedInputs.frame <= MAX_ROLLBACK_FRAMES) {
                            INFO_LOG_FMT(BRAWLBACK, "Using predicted inputs from frame {}\n", predictedInputs.frame);
                            // rekey predicted inputs for this frame
                            predictedInputs.frame = frame;
                            framedataToSendToGame.playerFrameDatas[playerIdx] = predictedInputs;
                            foundData.second = true;
                        }
                        else {
                            WARN_LOG_FMT(BRAWLBACK, "Prediction threshold reached! Predicted frame {}  current frame {} \n", predictedInputs.frame, frame);
                            // will timesync
                        }
                    }
                }

            }
            else {
                ERROR_LOG_FMT(BRAWLBACK, "Too early of a frame. Can't rollback. Using dummy pad\n");
                framedataToSendToGame.playerFrameDatas[playerIdx] = CreateDummyPlayerFrameData(frame, playerIdx);
            }
        }
        #else
        if (!foundData.second) { // didn't find framedata for this frame.
            ERROR_LOG_FMT(BRAWLBACK, "no remote framedata - frame {} remotePIdx {}\n", frame, playerIdx);
            framedataToSendToGame.playerFrameDatas[playerIdx] = CreateDummyPlayerFrameData(frame, playerIdx);
        }
        #endif
    }

    return foundData;
}


std::optional<Match::PlayerFrameData> CEXIBrawlback::HandleInputPrediction(u32 frame, u8 playerIdx) {
    // keep in mind the inputs returned here will be for a previous frame
    std::optional<Match::PlayerFrameData> ret = std::nullopt;

    INFO_LOG_FMT(BRAWLBACK, "Trying to find frame for predicted inputs...\n");

    // do we even need a loop here to find the inputs for prediction? I think they should always be the latest confirmed frame
    for (s32 frameIter = this->latestConfirmedFrame; frameIter < (int)frame; frameIter++) {
        // find most recent frame that exists
        Match::PlayerFrameData* predictedInputs = findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], frameIter);
        if (predictedInputs) {
            INFO_LOG_FMT(BRAWLBACK, "found frame for predicting inputs {}\n", frameIter);

            ret = std::make_optional(*predictedInputs);

            // when we begin predicting inputs, we find the most recent confirmed frame
            // so we should be rolling back to this frame + 1 since that's the most recent *non* confirmed frame
            s32 rollbackBeginFrame = frameIter + 1;
            // set rollback info

            // we are currently predicting
            this->rollbackInfo.isUsingPredictedInputs = true;
            // we begin our "rollback state" on the latest confirmed remote frame + 1
            this->rollbackInfo.beginFrame = rollbackBeginFrame;
            // store predicted inputs
            this->rollbackInfo.predictedInputs.playerFrameDatas[playerIdx] = *predictedInputs;

            break;
        }
    }

    if (!ret) {
        INFO_LOG_FMT(BRAWLBACK, "Searched [{}, {})   remote framedata range: {} - {}\n",
        this->latestConfirmedFrame, frame, this->remotePlayerFrameData[playerIdx].front()->frame, this->remotePlayerFrameData[playerIdx].back()->frame);
        // couldn't find relevant past framedata
        // this probably means the difference between clients is greater than MAX_ROLLBACK_FRAMES
        // foundData.second will be false, so this should time-sync later in handleLocalPadData
        WARN_LOG_FMT(BRAWLBACK, "Couldn't find framedata when we should rollback!! Will timesync\n");
    }

    return ret;
}



// prepares RollbackInfo struct with relevant rollback info
// currentFrame is the current frame we're on, and confirmFrame is the latest frame of inputs we just received
void CEXIBrawlback::SetupRollback(u32 currentFrame, u32 confirmFrame) {
    rollbackInfo.beginFrame = this->latestConfirmedFrame+1; // frame to rollback to
    this->latestConfirmedFrame = (int)confirmFrame; // frame of latest confirmed remote inputs
    this->rollbackInfo.endFrame = currentFrame; // current frame we need to resim up to

    INFO_LOG_FMT(BRAWLBACK, "Rollback confirmed frame range: [{}, {}] currentFrame: {}  localPidx {}\n", this->rollbackInfo.beginFrame, confirmFrame, currentFrame, this->localPlayerIdx);

    for (u32 i = this->rollbackInfo.beginFrame; i <= currentFrame; i++) {

        for (int pIdx = 0; pIdx < this->numPlayers; pIdx++) {

            const PlayerFrameDataQueue& inputQueue = pIdx == this->localPlayerIdx ? this->localPlayerFrameData : this->remotePlayerFrameData[pIdx];
            
            u32 frameDiff = i - this->rollbackInfo.beginFrame; // idx.  [0, MAX_ROLLBACK_FRAMES]
            ASSERT(frameDiff < MAX_ROLLBACK_FRAMES && frameDiff >= 0); // don't index out of bounds
            
            Match::PlayerFrameData* inputToPopulate = &this->rollbackInfo.pastFrameDatas[frameDiff].playerFrameDatas[pIdx];

            // inputs we have received / are local should just be copied into past framedatas
            if (RANGE(i, this->rollbackInfo.beginFrame, confirmFrame) || pIdx == this->localPlayerIdx) {
                Match::PlayerFrameData* pastInputs = findInPlayerFrameDataQueue(inputQueue, i);
                if (pastInputs) {
                    //INFO_LOG(BRAWLBACK, "Inserting inputs idx %u pidx %i frame %u\n", frameDiff, pIdx, pastInputs->frame);
                    memcpy(inputToPopulate, pastInputs, sizeof(Match::PlayerFrameData));
                }
                else {
                    ERROR_LOG_FMT(BRAWLBACK, "Couldn't find past inputs for rollback! Frame {} pidx {}\n", i, pIdx);
                }
            }
            // inputs we haven't received, and aren't local, and should still be predicted
            else {
                // remote inputs should be the predicted inputs
                Match::PlayerFrameData predictedInputs = this->rollbackInfo.predictedInputs.playerFrameDatas[pIdx];
                predictedInputs.frame = i; // rekey predicted inputs - pretending these are remote inputs
                predictedInputs.playerIdx = pIdx;
                //INFO_LOG(BRAWLBACK, "Inserting predicted inputs idx %u pidx %i frame %u\n", frameDiff, predictedInputs.playerIdx, predictedInputs.frame);
                *inputToPopulate = predictedInputs;
            }

        }

    }


    // this indicates that we should roll back on this frame
    this->rollbackInfo.pastFrameDataPopulated = true;

    #if 0
    // print rollbackInfo
    INFO_LOG(BRAWLBACK, "RbInfo: beginFrame %u  endFrame: %u\n", rollbackInfo.beginFrame, confirmFrame);
    for (int i = 0; i < MAX_ROLLBACK_FRAMES; i++) {
        const Match::FrameData& fd = rollbackInfo.pastFrameDatas[i];
        INFO_LOG(BRAWLBACK, "~~~~~~~ pastFramedatas[%i] ~~~~~~~\n", i);
        for (int pIdx = 0; pIdx < 2; pIdx++) {
            if (fd.playerFrameDatas[pIdx].frame != 0)
                INFO_LOG(BRAWLBACK, "pIdx %u:::   Frame %u\n", (unsigned int)fd.playerFrameDatas[pIdx].playerIdx, fd.playerFrameDatas[pIdx].frame);
        }
    }
    #endif

    #ifdef SYNCLOG
    for (int b = 0; b < MAX_ROLLBACK_FRAMES; b++) {
        if (rollbackInfo.pastFrameDatas[b].playerFrameDatas[0].frame <= confirmFrame)
            Sync::SyncLog(Sync::stringifyFramedata(rollbackInfo.pastFrameDatas[b], 2));
    }
    #endif

}


u32 CEXIBrawlback::GetLatestRemoteFrame() {
    u32 lowestFrame = 0;
	for (int i = 0; i < this->numPlayers; i++)
	{
        if (i == this->localPlayerIdx) continue;

		if (this->remotePlayerFrameData[i].empty())
		{
			return 0;
		}

		u32 f = this->remotePlayerFrameData[i].back()->frame;
		if (f < lowestFrame || lowestFrame == 0)
		{
			lowestFrame = f;
		}
	}

	return lowestFrame;
}

void BroadcastFramedataAck(u32 frame, u8 playerIdx, BrawlbackNetplay* netplay, ENetHost* server) {
    FrameAck ackData;
    ackData.frame = (int)frame;
    ackData.playerIdx = playerIdx;
    sf::Packet ackDataPacket = sf::Packet();
    u8 cmdbyte = NetPacketCommand::CMD_FRAME_DATA_ACK;
    ackDataPacket.append(&cmdbyte, sizeof(cmdbyte));
    ackDataPacket.append(&ackData, sizeof(FrameAck));
    netplay->BroadcastPacket(ackDataPacket, ENET_PACKET_FLAG_UNSEQUENCED, server);
    //INFO_LOG(BRAWLBACK, "Sent ack for frame %u  pidx %u", frame, (unsigned int)playerIdx);
}





void CEXIBrawlback::ProcessIndividualRemoteFrameData(Match::PlayerFrameData* framedata) {
    u8 playerIdx = framedata->playerIdx;
    u32 frame = framedata->frame;
    PlayerFrameDataQueue& remoteFramedataQueue = this->remotePlayerFrameData[playerIdx];

    if (!remoteFramedataQueue.empty() ) {
        // if the remote frame we're trying to process is not newer than the most recent frame, we don't care about it
        if (frame <= remoteFramedataQueue.back()->frame)
            return;
        // make sure the inputs we're adding are sequential
        ASSERT(frame == remoteFramedataQueue.back()->frame + 1); 
    }

    std::unique_ptr<Match::PlayerFrameData> f = std::make_unique<Match::PlayerFrameData>(*framedata);
    INFO_LOG_FMT(BRAWLBACK, "Received opponent framedata. Player {} frame: {} (w/o delay {})\n", (unsigned int)playerIdx, frame, frame-FRAME_DELAY);

    remoteFramedataQueue.push_back(std::move(f));

    // clamp size of remote player framedata queue
    while (remoteFramedataQueue.size() > FRAMEDATA_MAX_QUEUE_SIZE) {
        //WARN_LOG(BRAWLBACK, "Hit remote player framedata queue max size! %u\n", remoteFramedataQueue.size());
        remoteFramedataQueue.pop_front();
    }
}

void CEXIBrawlback::ProcessRemoteFrameData(Match::PlayerFrameData* framedatas, u8 numFramedatas_u8) {
    s32 numFramedatas = (s32)numFramedatas_u8;
    // framedatas may point to one or more PlayerFrameData's.
    // Also note. this array is the reverse of the local pad queue, in that
    // the 0th element here is the most recent framedata.
    Match::PlayerFrameData* mostRecentFramedata = &framedatas[0];
    u32 frame = mostRecentFramedata->frame;
    u8 playerIdx = mostRecentFramedata->playerIdx; // remote player idx

    // acknowledge that we received opponent's framedata
    BroadcastFramedataAck(frame, playerIdx, this->netplay.get(), this->server);
    // ---------------------

    //if (!this->remotePlayerFrameData[playerIdx].empty())
      //INFO_LOG(BRAWLBACK, "Received remote inputs. Head frame %u  received head frame %u\n", this->remotePlayerFrameData[playerIdx].back()->frame, frame);

    if (numFramedatas > 0) {
        std::lock_guard<std::mutex> lock (remotePadQueueMutex);

        std::stringstream s;
        s << "Received " << numFramedatas << " framedatas. [";
        for (int i = 0; i < numFramedatas; i++) {
            s << framedatas[i].frame << ", ";
        }
        s << "]";
        INFO_LOG_FMT(BRAWLBACK, "{}\n", s.str());

        u32 maxFrame = 0;
        // index 0 is most recent, and we want to process new framedata oldest first, then newer ones
        for (s32 i = numFramedatas-1; i >= 0; i--) {
            Match::PlayerFrameData* framedata = &framedatas[i];
            maxFrame = framedata->frame > maxFrame ? framedata->frame : maxFrame;
            this->ProcessIndividualRemoteFrameData(framedata);
        }

        this->timeSync->ReceivedRemoteFramedata(maxFrame, localPlayerIdx, this->hasGameStarted);

    }

}

void CEXIBrawlback::ProcessFrameAck(FrameAck* frameAck) {
    ASSERT(frameAck->playerIdx == this->localPlayerIdx); // should be local player
    this->timeSync->ProcessFrameAck(frameAck);
}

void CEXIBrawlback::ProcessGameSettings(Match::GameSettings* opponentGameSettings) {
    // merge game settings for all remote/local players, then pass that back to the game

    this->localPlayerIdx = this->isHost ? 0 : 1;
    // assumes 1v1
    int remotePlayerIdx = this->isHost ? 1 : 0;

    Match::GameSettings* mergedGameSettings = this->gameSettings.get();
    INFO_LOG_FMT(BRAWLBACK, "ProcessGameSettings for player {}\n", this->localPlayerIdx);
    INFO_LOG_FMT(BRAWLBACK, "Remote player idx: {}\n", remotePlayerIdx);

    mergedGameSettings->localPlayerIdx = this->localPlayerIdx;

    this->numPlayers = mergedGameSettings->numPlayers;
    INFO_LOG_FMT(BRAWLBACK, "Num players from emu: {}\n", (unsigned int)this->numPlayers);

    // this is kinda broken kinda unstable and weird.
    // hardcoded "fix" for testing. Get rid of this when you're confident this is stable
    if (this->numPlayers == 0) {
        this->numPlayers = 2;
        mergedGameSettings->numPlayers = 2;
    }

    if (!this->isHost) { // is not host
        mergedGameSettings->randomSeed = opponentGameSettings->randomSeed;

        // get a random stage on the non-host side.
        mergedGameSettings->stageID = matchmaking->GetRandomStage();

        // if not host, your character will be p2, if host, your char will be p1

        // copy char into both slots
        mergedGameSettings->playerSettings[1].charID = mergedGameSettings->playerSettings[0].charID;
        // copy char from opponent p1 into our p1
        mergedGameSettings->playerSettings[0].charID = opponentGameSettings->playerSettings[0].charID;
    }
    else { // is host
        // copy char from opponent's p2 into our p2
        mergedGameSettings->playerSettings[1].charID = opponentGameSettings->playerSettings[1].charID;

        // set our stage based on the one the other client generated
        mergedGameSettings->stageID = opponentGameSettings->stageID;
    }
    mergedGameSettings->playerSettings[localPlayerIdx].playerType = Match::PlayerType::PLAYERTYPE_LOCAL;
    mergedGameSettings->playerSettings[remotePlayerIdx].playerType = Match::PlayerType::PLAYERTYPE_REMOTE;

    // if we're not host, we just connected to host and received their game settings,
    // now we need to send our game settings back to them so they can start their game too
    if (!this->isHost) {
        this->netplay->BroadcastGameSettings(this->server, mergedGameSettings);
    }

    std::vector<u8> mergedGameSettingsByteVec = Mem::structToByteVector(mergedGameSettings);
    std::lock_guard<std::mutex> lock (read_queue_mutex);
    this->read_queue.push_back(EXICommand::CMD_SETUP_PLAYERS);
    this->read_queue.insert(this->read_queue.end(), mergedGameSettingsByteVec.begin(), mergedGameSettingsByteVec.end());
}


// called from netplay thread
void CEXIBrawlback::ProcessNetReceive(ENetEvent* event) {
    ENetPacket* pckt = event->packet;
    if (pckt && pckt->data && pckt->dataLength > 0) {
        u8* fullpckt_data = pckt->data;
        u8 cmd_byte = fullpckt_data[0];
        u8* data = &fullpckt_data[1];

        switch (cmd_byte) {
            case NetPacketCommand::CMD_FRAME_DATA:
                {
                    u8 numFramedatas = data[0];
                    Match::PlayerFrameData* framedata = (Match::PlayerFrameData*)(&data[1]);
                    this->ProcessRemoteFrameData(framedata, numFramedatas);
                }
                break;
            case NetPacketCommand::CMD_FRAME_DATA_ACK:
                {
                    FrameAck* frameAck = (FrameAck*)data;
                    this->ProcessFrameAck(frameAck);
                }
                break;
            case NetPacketCommand::CMD_GAME_SETTINGS:
                {
                    INFO_LOG_FMT(BRAWLBACK, "Received game settings from opponent");
                    Match::GameSettings* gameSettingsFromOpponent = (Match::GameSettings*)data;
                    this->ProcessGameSettings(gameSettingsFromOpponent);
                }
                break;
            default:
                WARN_LOG_FMT(BRAWLBACK, "Unknown packet cmd byte!");
                INFO_LOG_FMT(BRAWLBACK, "Packet as string: {}\n", reinterpret_cast<const char*>(fullpckt_data));
                break;
        }
    }
}




void CEXIBrawlback::NetplayThreadFunc() {
    Common::SetCurrentThreadName("BrawlbackNetplay");
    ENetEvent event;

    // loop until we connect to someone, then after we connected,
    // do another loop for passing data between the connected clients

    INFO_LOG_FMT(BRAWLBACK, "Waiting for connection to opponent...");
    while (enet_host_service(this->server, &event, 0) >= 0 && !this->isConnected) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                INFO_LOG_FMT(BRAWLBACK, "Connected!");
                if (event.peer) {
                    INFO_LOG_FMT(BRAWLBACK, "A new client connected from {:x}:{}\n",
                        event.peer -> address.host,
                        event.peer -> address.port);
                    this->isConnected = true;
                }
                else {
                    WARN_LOG_FMT(BRAWLBACK, "Connect event received, but peer was null!");
                }
                break;
            case ENET_EVENT_TYPE_NONE:
                //INFO_LOG(BRAWLBACK, "Enet event type none. Nothing to do");
                break;
        }
    }

    if (this->isHost) { // if we're host, send our game settings to clients right after connecting
        this->netplay->BroadcastGameSettings(this->server, this->gameSettings.get());
    }

    INFO_LOG_FMT(BRAWLBACK, "Starting main net data loop");
    // main enet loop
    while (enet_host_service(this->server, &event, 0) >= 0 && this->isConnected && !this->timeSync->getIsConnectionStalled()) {
        this->netplay->FlushAsyncQueue(this->server);
        switch (event.type) {
            case ENET_EVENT_TYPE_DISCONNECT:
                //INFO_LOG(BRAWLBACK, "%s:%u disconnected.\n", event.peer -> address.host, event.peer -> address.port);
                INFO_LOG_FMT(BRAWLBACK, "disconnected.\n");
                this->isConnected = false;
                break;
            case ENET_EVENT_TYPE_NONE:
                //INFO_LOG(BRAWLBACK, "Enet event type none. Nothing to do");
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                this->ProcessNetReceive(&event);
                enet_packet_destroy(event.packet);
                break;
        }
    }

    ERROR_LOG_FMT(BRAWLBACK, "~~~~~~~~~~~~~ END ENET THREAD ~~~~~~~~~~~~~~~");
}

void CEXIBrawlback::MatchmakingThreadFunc()
{
  Common::SetCurrentThreadName("BrawlbackMatchmakingPhase2");
  while (this->matchmaking)
  {
    switch (this->matchmaking->GetMatchmakeState())
    {
    case Matchmaking::ProcessState::OPPONENT_CONNECTING:
      this->matchmaking->SetMatchmakeState(Matchmaking::ProcessState::CONNECTION_SUCCESS);
      this->connectToOpponent();
      break;
    case Matchmaking::ProcessState::ERROR_ENCOUNTERED:
      ERROR_LOG_FMT(BRAWLBACK, "MATCHMAKING: ERROR TRYING TO CONNECT!");
      return;
      break;
    default:
      break;
    }
  }
  INFO_LOG_FMT(BRAWLBACK, "~~~~~~~~~~~~~~ END MATCHMAKING THREAD ~~~~~~~~~~~~~~\n");
}

void CEXIBrawlback::connectToOpponent() {
  this->isHost = this->matchmaking->IsHost();

  if(this->isHost) {
    INFO_LOG_FMT(BRAWLBACK, "Matchmaking: Creating server...");
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = this->matchmaking->GetLocalPort();

    this->server = enet_host_create(&address, 10, 3, 0, 0);

  } else {
    INFO_LOG_FMT(BRAWLBACK, "Matchmaking: Creating client...");
    this->server = enet_host_create(NULL, 10, 3, 0, 0);

    bool connectedToAtLeastOne = false;
    for(int i=0; i < this->matchmaking->RemotePlayerCount(); i++)
    {
      ENetAddress addr;
      int set_host_res = enet_address_set_host(&addr, this->matchmaking->GetRemoteIPAddresses()[i].c_str());
      if (set_host_res < 0) {
        WARN_LOG_FMT(BRAWLBACK, "Failed to enet_address_set_host");
      }
      addr.port = this->matchmaking->GetRemotePorts()[i];

      ENetPeer* peer = enet_host_connect(this->server, &addr, 1, 0);
      if (peer == NULL) {
        WARN_LOG_FMT(BRAWLBACK, "Failed to enet_host_connect");
      }
      connectedToAtLeastOne = true;
    }
    if(!connectedToAtLeastOne) {
      ERROR_LOG_FMT(BRAWLBACK, "Failed to connect to any client/host");
      return;
    }
  }


  INFO_LOG_FMT(BRAWLBACK, "Net initialized, starting netplay thread");
  // loop to receive data over net
  this->netplay_thread = std::thread(&CEXIBrawlback::NetplayThreadFunc, this);
}

void CEXIBrawlback::handleFindMatch(u8* payload) {
    //if (!payload) return;
  #define LOCAL_TESTING
#ifdef LOCAL_TESTING
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = BRAWLBACK_PORT;

    this->server = enet_host_create(&address, 3, 0, 0, 0);

#define IP_FILENAME "/connect.txt"
    std::string connectIP = "127.0.0.1";

    if (File::Exists(File::GetExeDirectory() + IP_FILENAME))
    {
      std::fstream file;
      File::OpenFStream(file, File::GetExeDirectory() + IP_FILENAME, std::ios_base::in);
      connectIP.clear();
      std::getline(file, connectIP);  // read in only one line
      file.close();
      INFO_LOG_FMT(BRAWLBACK, "IP: {}\n", connectIP);
    }
    else
    {
      INFO_LOG_FMT(BRAWLBACK, "Creating connect file\n");
      std::fstream file;
      file.open(File::GetExeDirectory() + IP_FILENAME, std::ios_base::out);
      file << connectIP;
      file.close();
    }

    // just for testing. This should be replaced with a check to see if we are the "host" of the
    // match or not
    if (this->server == NULL)
    {
      this->isHost = false;
      WARN_LOG_FMT(BRAWLBACK, "Failed to init enet server!");
      WARN_LOG_FMT(BRAWLBACK, "Creating client instead...");
      this->server = enet_host_create(NULL, 3, 0, 0, 0);
      // for (int i = 0; i < 1; i++) { // make peers for all connecting opponents

      ENetAddress addr;
      int set_host_res = enet_address_set_host(&addr, connectIP.c_str());
      if (set_host_res < 0)
      {
        WARN_LOG_FMT(BRAWLBACK, "Failed to enet_address_set_host");
        return;
      }
      addr.port = BRAWLBACK_PORT;

      ENetPeer* peer = enet_host_connect(this->server, &addr, 1, 0);
      if (peer == NULL)
      {
        WARN_LOG_FMT(BRAWLBACK, "Failed to enet_host_connect");
        return;
      }

      //}
    }

    INFO_LOG_FMT(BRAWLBACK, "Net initialized, starting netplay thread");
    // loop to receive data over net
    this->netplay_thread = std::thread(&CEXIBrawlback::NetplayThreadFunc, this);
#else

    Matchmaking::MatchSearchSettings search;
    std::string connectCode;

    // TODO: uncomment these lines when payload includes the actual mode and connect codes
#ifdef REMOVE_THIS_WHEN_PAYLOAD_IS_SET
    search.mode = (SlippiMatchmaking::OnlinePlayMode)payload[0];
    std::string shiftJisCode;
    shiftJisCode.insert(shiftJisCode.begin(), &payload[1], &payload[1] + 18);
    shiftJisCode.erase(std::find(shiftJisCode.begin(), shiftJisCode.end(), 0x00), shiftJisCode.end());
    connectCode = shiftJisCode;
#else
  search.mode = Matchmaking::OnlinePlayMode::UNRANKED;
#endif

    switch (search.mode)
    {
    case Matchmaking::OnlinePlayMode::DIRECT:
    case Matchmaking::OnlinePlayMode::TEAMS:
      search.connectCode = connectCode;
      break;
    default:
      break;
    }

    // Store this search so we know what was queued for
    lastSearch = search;
    matchmaking->FindMatch(search);
    this->matchmaking_thread = std::thread(&CEXIBrawlback::MatchmakingThreadFunc, this);
#endif
}


void CEXIBrawlback::handleRegisterExcludeRegion(u8* data)
{
    // Payload: [virt_addr u32 BE][size_bytes u32 BE]
    int idx = 0;
    const u32 virt_addr  = SlippiUtility::Mem::readWord(data, idx, 999, 0);
    const u32 size_bytes = SlippiUtility::Mem::readWord(data, idx, 999, 0);
    INFO_LOG_FMT(BRAWLBACK, "RegisterExcludeRegion: virt={:#010x} size={:#x}", virt_addr, size_bytes);
    Rollback::RollbackManager::Get().AddExcludeRegion(virt_addr, size_bytes);
}

void CEXIBrawlback::handleStartMatch(u8* payload) {
    //if (!payload) return;
    Match::GameSettings* settings = (Match::GameSettings*)payload;
    this->gameSettings = std::make_unique<Match::GameSettings>(*settings);
}


// recieve data from game into emulator
void CEXIBrawlback::DMAWrite(u32 address, u32 size)
{
    //INFO_LOG(BRAWLBACK, "DMAWrite size: %u\n", size);
    u8* mem = m_system.GetMemory().GetPointerForRange(address, size);

    if (!mem)
    {
        INFO_LOG_FMT(BRAWLBACK, "Invalid address in DMAWrite!");
        //this->read_queue.clear();
        return;
    }

    u8 command_byte = mem[0];  // first byte is always cmd byte
    u8* payload = &mem[1];     // rest is payload

    // no payload
    if (size <= 1)
        payload = nullptr;

#ifdef HAVE_TRACY
    static TracyCZoneCtx gameSimFrameTracyZone = {};
    static TracyCZoneCtx fullFrameTracyZone = {};
#endif
    static u64 frameTime = Common::Timer::NowUs();
    switch (command_byte)
    {

    case CMD_UNKNOWN:
        INFO_LOG_FMT(BRAWLBACK, "Unknown DMAWrite command byte!");
        break;
    case CMD_ONLINE_INPUTS:
        //INFO_LOG(BRAWLBACK, "DMAWrite: CMD_ONLINE_INPUTS");
        handleLocalPadData(payload);
        break;
    case CMD_CAPTURE_SAVESTATE:
        //INFO_LOG(BRAWLBACK, "DMAWrite: CMD_CAPTURE_SAVESTATE");
        handleCaptureSavestate(payload);
        break;
    case CMD_LOAD_SAVESTATE:
        //INFO_LOG(BRAWLBACK, "DMAWrite: CMD_LOAD_SAVESTATE");
        handleLoadSavestate(payload);
        break;
    case CMD_FIND_OPPONENT:
        //INFO_LOG(BRAWLBACK, "DMAWrite: CMD_FIND_OPPONENT");
        handleFindMatch(payload);
        break;
    case CMD_START_MATCH:
        //INFO_LOG(BRAWLBACK, "DMAWrite: CMD_START_MATCH");
        handleStartMatch(payload);
        break;
    case CMD_REGISTER_EXCLUDE_REGION:
        handleRegisterExcludeRegion(payload);
        break;

    // just using these CMD's to track frame times lol
    case CMD_TIMER_START:
        {
            frameTime = Common::Timer::NowUs();
#ifdef HAVE_TRACY
            static const struct ___tracy_source_location_data s_frame_loc{
                "BrawlbackFrame", __func__, __FILE__, (uint32_t)__LINE__, 0};
            fullFrameTracyZone = ___tracy_emit_zone_begin(&s_frame_loc, 1);
#endif
        }
        break;
    case CMD_TIMER_END:
        {
            u32 timeDiff = Common::Timer::NowUs() - frameTime;
            INFO_LOG_FMT(BRAWLBACK, "Game logic took {} ms\n", (double)(timeDiff / 1000.0));
#ifdef HAVE_TRACY
            TracyCZoneEnd(fullFrameTracyZone);
            TracyCFrameMark;
#endif
        }
        break;

      case CMD_GAMESIM_UPDATE:
      {
#ifdef HAVE_TRACY
        static const struct ___tracy_source_location_data s_frame_loc{
            "GameSimFrame", __func__, __FILE__, (uint32_t)__LINE__, 0};
        gameSimFrameTracyZone = ___tracy_emit_zone_begin(&s_frame_loc, 1);
#endif
      }
      break;
      case CMD_GAMESIM_UPDATE_END:
      {
#ifdef HAVE_TRACY
        TracyCZoneEnd(gameSimFrameTracyZone);
#endif
      }
      break;
    

    default:
        //INFO_LOG(BRAWLBACK, "Default DMAWrite %u\n", (unsigned int)command_byte);
        break;
  }

}

// send data from emulator to game
void CEXIBrawlback::DMARead(u32 address, u32 size)
{
    std::lock_guard<std::mutex> lock(read_queue_mutex);

    if (this->read_queue.empty()) {// we have nothing to send to the game
        this->read_queue.push_back(EXICommand::CMD_UNKNOWN); // result code
    }

    // game is trying to get cmd byte
    if (size == 1) {
        m_system.GetMemory().CopyToEmu(address, &this->read_queue[0], size);
        this->read_queue.erase(this->read_queue.begin());
        return;
    }

    this->read_queue.resize(size, 0);
    auto qAddr = &this->read_queue[0];
    m_system.GetMemory().CopyToEmu(address, qAddr, size);
    this->read_queue.clear();

}





// honestly dunno why these are overriden like this, but slippi does it sooooooo  lol
bool CEXIBrawlback::IsPresent() const
{
    return true;
}

void CEXIBrawlback::TransferByte(u8& byte) { }
