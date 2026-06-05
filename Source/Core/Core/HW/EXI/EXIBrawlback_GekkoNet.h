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

enum GekkoNetCmd : u8
{
    GKK_CMD_UNKNOWN   = 0,
    GKK_CMD_FRAME     = 20,
    GKK_FIND_OPPONENT = 5,
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
static constexpr u32 BRAWL_GAMEPROC_ADDR      = 0x80017618;
static constexpr u32 BRAWL_GF_APPLICATION_PTR  = 0x8059ffac;
// gfPadSystem instance at 0x805bacc0, raw pads at +0x40, stride 0x40
static constexpr u32 BRAWL_PADSYSTEM_INSTANCE  = 0x805bacc0;
static constexpr u32 BRAWL_PAD_RAW_BASE        = 0x805bacc0 + 0x40;
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

private:
    std::vector<u8> m_read_queue;

    GekkoSession* m_session = nullptr;
    int m_local_handle  = -1;
    int m_remote_handle = -1;
    std::string m_remote_addr_str;

    bool m_rb_initialized  = false;
    bool m_session_started = false;
    int  m_local_player_idx = 0;
    u8   m_num_players      = 2;
    bool m_is_host          = true;
    int  m_current_frame    = 0;
    std::unique_ptr<Match::GameSettings> m_game_settings;

    std::unique_ptr<Matchmaking> m_matchmaking;
    std::thread m_matchmaking_thread;

    // SI override state
    static inline std::atomic<bool> s_override_active{false};
    static inline GCPadStatus s_override_pads[MAX_NUM_PLAYERS]{};

    // Handlers
    void HandleFrame(u8* payload);
    void HandleFindOpponent(u8* payload);
    void HandleStartMatch(u8* payload);
    void HandleRegisterExclude(u8* payload);

    void InitGekkoSession(const std::string& remote_addr, unsigned short local_port);
    void DestroyGekkoSession();

    // PPC calling
    void RunPPCFunction(u32 addr, u32 r3 = 0, u32 r4 = 0);
    void RunGameProc(int resim_index);

    // Input injection (dual: SI override + direct memory write)
    void InjectPads(const BrawlbackPad pads[MAX_NUM_PLAYERS]);

    void ExtractPads(const unsigned char* gekko_inputs, BrawlbackPad out[MAX_NUM_PLAYERS]) const;
    UserInfo GetUserInfo() const;
    void MatchmakingThread();

protected:
    void TransferByte(u8& byte) override;
};
