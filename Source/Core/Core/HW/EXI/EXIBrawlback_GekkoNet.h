#pragma once

#include "Core/HW/EXI/EXI_Device.h"
#include "Core/Brawlback/BrawlbackUtility.h"
#include "Core/Brawlback/Netplay/Matchmaking.h"
#include "Core/System.h"

#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC
#endif
#include "gekkonet.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Brawlback;

#define BRAWLBACK_USE_GEKKONET 1

namespace Core
{
class CPUThreadGuard;
}

enum GekkoNetCmd : u8
{
    GKK_CMD_UNKNOWN   = 0,
    GKK_END_MATCH     = 11,
    GKK_START_MATCH   = 13,
    GKK_SETUP_PLAYERS = 14,
    GKK_REG_EXCLUDE   = 18,
};

#pragma pack(push, 1)
struct GKKFramePayload
{
    u32 frame;
    BrawlbackPad pad;
};
#pragma pack(pop)

// Brawl addresses
static constexpr u32 BRAWL_GAME_LOOP_HOOK_ADDR = 0x80017344;
static constexpr u32 BRAWL_GAME_LOOP_CONDITION_ADDR = 0x800173a4;
static constexpr u32 BRAWL_GAMEPROC_CALLSITE_ADDR = 0x80017350;
static constexpr u32 BRAWL_GAMEPROC_CALLSITE_NEXT_ADDR = 0x80017354;
// gfPadSystem instance at 0x805bacc0, raw pads at +0x40, stride 0x40
static constexpr u32 BRAWL_PADSYSTEM_INSTANCE  = 0x805bacc0;
static constexpr u32 BRAWL_PAD_RAW_BASE = BRAWL_PADSYSTEM_INSTANCE + 0x40;
static constexpr u32 BRAWL_PAD_STRIDE          = 0x40;
// gfPadGamecube layout: buttons at +0x06 (u16), sticks at +0x30 (6 bytes)
static constexpr u32 PAD_OFF_BUTTONS = 0x06;
static constexpr u32 PAD_OFF_STICKS  = 0x30;

class CEXIBrawlbackGekkoNet : public ExpansionInterface::IEXIDevice
{
public:
    explicit CEXIBrawlbackGekkoNet(Core::System& system);
    ~CEXIBrawlbackGekkoNet() override;

    void DMAWrite(u32 address, u32 size) override;
    void DMARead(u32 address, u32 size) override;
    bool IsPresent() const override;
    void DoState(PointerWrap& p) override;

    // SI override — called from CSIDevice_GCController::GetPadStatus
    static bool GetOverrideInput(int pad_num, GCPadStatus* status);
    static void GameLoopHook(const Core::CPUThreadGuard& guard);
    static void GameProcCallsiteHook(const Core::CPUThreadGuard& guard);

private:
    std::vector<u8> m_read_queue;

    GekkoSession* m_session = nullptr;
    int m_local_handle  = -1;
    int m_remote_handle = -1;
    std::string m_remote_addr_str;
    unsigned short m_local_port = 0;
    bool m_enet_initialized = false;

    bool m_rb_initialized  = false;
    bool m_session_started = false;
    bool m_seen_match_frame_zero = false;
    int  m_local_player_idx = 0;
    u8   m_num_players      = 2;
    bool m_is_host          = true;
    int  m_current_frame    = 0;
    int  m_connect_wait_ticks = 0;
    int  m_pending_adv_count = 0;
    BrawlbackPad m_pending_adv_pads[MAX_ROLLBACK_FRAMES + 1][MAX_NUM_PLAYERS]{};
    std::unique_ptr<Match::GameSettings> m_game_settings;

    std::unique_ptr<Matchmaking> m_matchmaking;
    std::thread m_matchmaking_thread;

    static inline CEXIBrawlbackGekkoNet* s_active_device = nullptr;

    // SI override state
    static inline std::atomic<bool> s_override_active{false};
    static inline GCPadStatus s_override_pads[MAX_NUM_PLAYERS]{};

    // Handlers
    int HandleFrame(u8* payload);
    bool ShouldControlGameLoop() const;
    void RunDolphinControlledGameLoop(const Core::CPUThreadGuard& guard);
    void RunGameProcCallsiteHook(const Core::CPUThreadGuard& guard);
    void HandleFindOpponent(u8* payload);
    void HandleStartMatch(u8* payload);
    void HandleEndMatch();
    void HandleRegisterExclude(u8* payload);

    void InitGekkoSession(const std::string& remote_addr, unsigned short local_port);
    void DestroyGekkoSession();
    bool ExchangeGameSettings(Match::GameSettings* remote_settings) const;
    void MergeGameSettings(const Match::GameSettings& remote_settings);
    void QueueGameSettingsResponse();

    // Input injection (dual: SI override + direct memory write)
    void InjectPads(const BrawlbackPad pads[MAX_NUM_PLAYERS]);
    GKKFramePayload ReadFramePayloadFromGameMemory() const;

    void ExtractPads(const unsigned char* gekko_inputs, BrawlbackPad out[MAX_NUM_PLAYERS]) const;
    UserInfo GetUserInfo() const;
    void MatchmakingThread();

protected:
    void TransferByte(u8& byte) override;
};
