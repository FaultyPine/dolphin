
#include "EXIBrawlback_GekkoNet.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/RollbackManager.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/OnScreenDisplay.h"
#include <Core/Rollback/Perf.h>

#include "Core/Brawlback/include/json.hpp"
using json = nlohmann::json;

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>

// ---- Construction / Destruction ----

CEXIBrawlbackGekkoNet::CEXIBrawlbackGekkoNet(Core::System& system) : IEXIDevice(system)
{
    // Truncate the log file so each session starts fresh.
    {
      const std::string log_path = File::GetUserPath(F_MAINLOG_IDX);
      std::ofstream ofs(log_path, std::ios::trunc);
    }
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet EXI device created");
    m_enet_initialized = enet_initialize() == 0;
    if (!m_enet_initialized)
        ERROR_LOG_FMT(BRAWLBACK, "GekkoNet: failed to initialize ENet");
    ASSERT(s_active_device == nullptr);
    s_active_device = this;
    m_matchmaking = std::make_unique<Matchmaking>(GetUserInfo());
}

CEXIBrawlbackGekkoNet::~CEXIBrawlbackGekkoNet()
{
    if (s_active_device == this)
        s_active_device = nullptr;
    HLE::UnPatch(m_system, "BrawlbackGekkoNetGameLoop");
    HLE::UnPatch(m_system, "BrawlbackGekkoNetGameProcCallsite");
    HLE::UnPatch(m_system, "BrawlbackGekkoNetClearPadEdgeCallsite");
    s_override_active = false;
    DestroyGekkoSession();
    if (m_matchmaking_thread.joinable())
        m_matchmaking_thread.join();
    Rollback::RollbackManager::Get().Shutdown();
    if (m_enet_initialized)
        enet_deinitialize();
}

// ---- DMA ----

void CEXIBrawlbackGekkoNet::DMAWrite(u32 address, u32 size)
{
    u8* mem = m_system.GetMemory().GetPointerForRange(address, size);
    if (!mem) return;
    u8 cmd = mem[0];
    u8* payload = size > 1 ? &mem[1] : nullptr;
    switch (cmd)
    {
    case GKK_END_MATCH:       HandleEndMatch();               break;
    case GKK_START_MATCH:     HandleStartMatch(payload);      break;
    case GKK_REG_EXCLUDE:     HandleRegisterExclude(payload); break;
    default: break;
    }
}

void CEXIBrawlbackGekkoNet::DMARead(u32 address, u32 size)
{
    if (m_read_queue.empty())
        m_read_queue.push_back(GKK_CMD_UNKNOWN);
    if (size == 1)
    {
        m_system.GetMemory().CopyToEmu(address, &m_read_queue[0], 1);
        m_read_queue.erase(m_read_queue.begin());
        return;
    }
    m_read_queue.resize(size, 0);
    m_system.GetMemory().CopyToEmu(address, m_read_queue.data(), size);
    m_read_queue.clear();
}

bool CEXIBrawlbackGekkoNet::IsPresent() const { return true; }
void CEXIBrawlbackGekkoNet::TransferByte(u8&) {}
void CEXIBrawlbackGekkoNet::DoState(PointerWrap&) {}

// ---- mainLoopSub gameProc loop ownership ----

/*
// <GameLoopHook>
for (int i = 0; i < numTicksToAdvance; i++)
{
  // <GameProcCallsiteHook>
  int res = gameProc(gfApplication, i);
  if (i == 0 && some other condition idk)
  {
    clearPadEdgeRepert(gfPadSystem); // clears buffered inputs when simulating multiple frames at once, so we don't trigger 2 inputs in the same frame
  }
  // .... some other stuff that doesn't rlly matter i think?
}
*/
void CEXIBrawlbackGekkoNet::GameLoopHook(const Core::CPUThreadGuard& guard)
{
    if (!s_active_device || !s_active_device->ShouldControlGameLoop())
    {
        auto& ppc_state = guard.GetSystem().GetPPCState();
        ppc_state.gpr[25] = 0;  // original instruction at 0x80017344: li r25, 0
        ppc_state.npc = BRAWL_GAME_LOOP_HOOK_ADDR + 4;
        return;
    }
    s_active_device->RunDolphinControlledGameLoop(guard);
}

bool CEXIBrawlbackGekkoNet::ShouldControlGameLoop() const
{
    return m_game_settings != nullptr;
}

void CEXIBrawlbackGekkoNet::RunDolphinControlledGameLoop(const Core::CPUThreadGuard& guard)
{
    auto& ppc_state = guard.GetSystem().GetPPCState();
    s_override_active = false;
    const int adv_count = HandleFrame(nullptr);
    ppc_state.gpr[25] = 0;
    ppc_state.gpr[19] = 0;
    ppc_state.gpr[24] = static_cast<u32>(adv_count);
    ppc_state.npc = BRAWL_GAME_LOOP_CONDITION_ADDR;
}

void CEXIBrawlbackGekkoNet::GameProcCallsiteHook(const Core::CPUThreadGuard& guard)
{
    if (!s_active_device || !s_active_device->ShouldControlGameLoop())
    {
        auto& ppc_state = guard.GetSystem().GetPPCState();
        ppc_state.gpr[3] = ppc_state.gpr[23];  // original 0x80017350: or r3, r23, r23
        ppc_state.npc = BRAWL_GAMEPROC_CALLSITE_NEXT_ADDR;
        return;
    }
    s_active_device->RunGameProcCallsiteHook(guard);
}

void CEXIBrawlbackGekkoNet::RunGameProcCallsiteHook(const Core::CPUThreadGuard& guard)
{
    auto& ppc_state = guard.GetSystem().GetPPCState();
    const int resim_index = static_cast<int>(ppc_state.gpr[19]);

    if (resim_index >= 0 && resim_index < m_pending_ops.adv_count)
    {
        if (m_rb_initialized && resim_index > 0)
            Rollback::RollbackManager::Get().SaveFrame(m_system);
        InjectPads(m_pending_ops.adv_pads[resim_index]);
    }
    else if (m_pending_ops.adv_count > 0)
    {
        WARN_LOG_FMT(BRAWLBACK, "GekkoNet: gameProc callsite without pending input index={} count={}",
                     resim_index, m_pending_ops.adv_count);
    }

    ppc_state.gpr[3] = ppc_state.gpr[23];  // original 0x80017350: or r3, r23, r23
    ppc_state.npc = BRAWL_GAMEPROC_CALLSITE_NEXT_ADDR;
}

void CEXIBrawlbackGekkoNet::ClearPadEdgeCallsiteHook(const Core::CPUThreadGuard& guard)
{
    if (!s_active_device || !s_active_device->ShouldControlGameLoop())
    {
        auto& ppc_state = guard.GetSystem().GetPPCState();
        ppc_state.gpr[3] = ppc_state.gpr[26];  // original 0x80017370: or r3, r26, r26
        ppc_state.npc = BRAWL_CLEAR_PAD_EDGE_CALL_ADDR;
        return;
    }
    s_active_device->RunClearPadEdgeCallsiteHook(guard);
}

void CEXIBrawlbackGekkoNet::RunClearPadEdgeCallsiteHook(const Core::CPUThreadGuard& guard)
{
    guard.GetSystem().GetPPCState().npc = BRAWL_CLEAR_PAD_EDGE_CALL_NEXT_ADDR;
}

// ---- Input injection ----
// Dual path: SI override (catches PADRead if it fires during SingleStep)
//            + direct memory write to gfPadSystem raw buffer (guaranteed path).
//
// Input chain in Brawl:
//   PADRead (pad thread, alarm-driven) → SI device → writes to pads[+0x40]
//   gameProc → ipSwitch::update → updateGame → copies pads[+0x40] → processed[+0x444]
//                                → getGamePadStatus → reads processed[+0x444]
//
// The callsite hook writes the raw buffer immediately before Brawl's real gameProc call.

bool CEXIBrawlbackGekkoNet::GetOverrideInput(int pad_num, GCPadStatus* status)
{
    if (!s_override_active || pad_num < 0 || pad_num >= MAX_NUM_PLAYERS)
        return false;
    *status = s_override_pads[pad_num];
    return true;
}

void CEXIBrawlbackGekkoNet::InjectPads(const BrawlbackPad pads[MAX_NUM_PLAYERS])
{
    auto& mem = m_system.GetMemory();

    for (int i = 0; i < MAX_NUM_PLAYERS; i++)
    {
        // SI override (backup: catches PADRead if it fires during SingleStep)
        GCPadStatus& s = s_override_pads[i];
        s = {};
        s.button       = pads[i].buttons;
        s.stickX       = static_cast<u8>(static_cast<int>(pads[i].stickX) +
                                   GCPadStatus::MAIN_STICK_CENTER_X);
        s.stickY       = static_cast<u8>(static_cast<int>(pads[i].stickY) +
                                   GCPadStatus::MAIN_STICK_CENTER_Y);
        s.substickX    = static_cast<u8>(static_cast<int>(pads[i].cStickX) +
                                      GCPadStatus::C_STICK_CENTER_X);
        s.substickY    = static_cast<u8>(static_cast<int>(pads[i].cStickY) +
                                      GCPadStatus::C_STICK_CENTER_Y);
        s.triggerLeft  = static_cast<u8>(pads[i].LTrigger);
        s.triggerRight = static_cast<u8>(pads[i].RTrigger);
        s.isConnected  = (i < m_num_players);

        // Direct write to gfPadSystem raw pad buffer (primary path)
        // updateGame (called inside gameProc→ipSwitch::update) copies from here.
        u32 base = BRAWL_PAD_RAW_BASE + static_cast<u32>(i) * BRAWL_PAD_STRIDE;
        u16 buttons_be = Common::swap16(pads[i].buttons);
        mem.CopyToEmu(base + PAD_OFF_BUTTONS, &buttons_be, 2);
        u8 sticks[6] = {
            static_cast<u8>(pads[i].stickX),  static_cast<u8>(pads[i].stickY),
            static_cast<u8>(pads[i].cStickX), static_cast<u8>(pads[i].cStickY),
            static_cast<u8>(pads[i].LTrigger),static_cast<u8>(pads[i].RTrigger),
        };
        mem.CopyToEmu(base + PAD_OFF_STICKS, sticks, 6);
    }
    s_override_active = true;
}

GKKFramePayload CEXIBrawlbackGekkoNet::ReadFramePayloadFromGameMemory() const
{
    GKKFramePayload payload{};
    auto& mem = m_system.GetMemory();
    payload.frame = Rollback::ReadBrawlMatchFrameCounter(mem.GetEXRAM(), mem.GetExRamSize());

    const GCPadStatus status = Pad::GetStatus(0);
    payload.pad.buttons = status.button;
    payload.pad.stickX = static_cast<char>(static_cast<int>(status.stickX) -
                                           GCPadStatus::MAIN_STICK_CENTER_X);
    payload.pad.stickY = static_cast<char>(static_cast<int>(status.stickY) -
                                           GCPadStatus::MAIN_STICK_CENTER_Y);
    payload.pad.cStickX = static_cast<char>(static_cast<int>(status.substickX) -
                                            GCPadStatus::C_STICK_CENTER_X);
    payload.pad.cStickY = static_cast<char>(static_cast<int>(status.substickY) -
                                            GCPadStatus::C_STICK_CENTER_Y);
    payload.pad.LTrigger = static_cast<char>(status.triggerLeft);
    payload.pad.RTrigger = static_cast<char>(status.triggerRight);
    return payload;
}

// ---- Core per-frame handler ----
// Called once per mainLoopSub iteration from the HLE loop hook.
// LoadFrame does NOT restore PPC state → resim happens inline, no re-entry.

int CEXIBrawlbackGekkoNet::HandleFrame(u8* payload)
{
    m_pending_ops.Clear();
    GKKFramePayload local_payload = payload ? *reinterpret_cast<GKKFramePayload*>(payload) :
                                             ReadFramePayloadFromGameMemory();
    auto* p = &local_payload;
    int frame = payload ? static_cast<int>(Common::swap32(p->frame)) :
                          static_cast<int>(p->frame);
    //INFO_LOG_FMT(BRAWLBACK, "GekkoNet: frame {}", frame);
    m_current_frame = frame;
    auto& rbMgr = Rollback::RollbackManager::Get();

    if (!m_rb_initialized)
    {
        rbMgr.Init(m_system);
        m_rb_initialized = true;
    }

    if (!m_session)
    {
        return 1;
    }

    int gc = 0;
    GekkoGameEvent** ge = nullptr;
    const auto process_session_events = [&]() {
        int sc = 0;
        auto** se = gekko_session_events(m_session, &sc);
        for (int i = 0; i < sc; i++)
        {
            switch (se[i]->type)
            {
            case GekkoPlayerSyncing:
                INFO_LOG_FMT(BRAWLBACK, "GekkoNet: player {} syncing {}/{}",
                             se[i]->data.syncing.handle, se[i]->data.syncing.current,
                             se[i]->data.syncing.max);
                break;
            case GekkoPlayerConnected:
                INFO_LOG_FMT(BRAWLBACK, "GekkoNet: player {} connected", se[i]->data.connected.handle);
                break;
            case GekkoSessionStarted:
                m_session_started = true;
                m_connect_wait_ticks = 0;
                INFO_LOG_FMT(BRAWLBACK, "GekkoNet: session started");
                break;
            case GekkoPlayerDisconnected:
                WARN_LOG_FMT(BRAWLBACK, "GekkoNet: player {} disconnected",
                             se[i]->data.disconnected.handle);
                return false;
            case GekkoDesyncDetected:
                if (se[i]->data.desynced.frame <= GAME_FULL_START_FRAME)
                    break;
                WARN_LOG_FMT(BRAWLBACK,
                             "GekkoNet: desync frame={} remote={} local_checksum={:08x} remote_checksum={:08x}",
                             se[i]->data.desynced.frame, se[i]->data.desynced.remote_handle,
                             se[i]->data.desynced.local_checksum,
                             se[i]->data.desynced.remote_checksum);
                return false;
            default:
                break;
            }
        }
        return true;
    };

    const bool brawl_frame_started = frame > 0;
    if (!m_session_started)
    {
        ge = gekko_update_session(m_session, &gc);
        if (!process_session_events())
            return 0;
    }

    if (!m_session_started)
    {
        if (++m_connect_wait_ticks % 120 == 0)
            INFO_LOG_FMT(BRAWLBACK, "GekkoNet: waiting for session start remote={} local_handle={} remote_handle={}",
                         m_remote_addr_str, m_local_handle, m_remote_handle);
        return brawl_frame_started ? 0 : 1;
    }

    if (!m_seen_match_frame_zero)
    {
        gekko_network_poll(m_session);
        if (frame == 0)
            m_seen_match_frame_zero = true;
        return 1;
    }

    if (!brawl_frame_started)
    {
        gekko_network_poll(m_session);
        if (!process_session_events())
            return 0;
        return 1;
    }

    const bool rollback_ready = frame >= GAME_FULL_START_FRAME;
    if (!rollback_ready)
    {
        gekko_network_poll(m_session);
        if (!process_session_events())
            return 0;
        return 1;
    }

    if (frame == GAME_FULL_START_FRAME)
        INFO_LOG_FMT(BRAWLBACK, "GekkoNet: starting gameplay sync at brawl frame {}", frame);

    // intentional extra poll here to catch inputs that arrived between prev frame's update_session and this upcoming one
    gekko_network_poll(m_session);
    gekko_add_local_input(m_session, m_local_handle, &p->pad);

    ge = gekko_update_session(m_session, &gc);
    if (!process_session_events())
        return 0;

    bool has_load   = false;
    int  load_frame = 0;
    int  num_adv    = 0;
    BrawlbackPad adv_pads[MAX_ROLLBACK_FRAMES + 1][MAX_NUM_PLAYERS] = {};

    // NOTE: because I wasn't able to figure out how to call brawl's gameProc from dolphin without things blowing up (interrupts in nested JIT calls i think was the trigger)
    // we instead opt to do this sort of thing, where any gekkonet messages get queued up to be serviced later during our hooks
    for (int i = 0; i < gc; i++)
    {
        auto& e = *ge[i];
        switch (e.type)
        {
        case GekkoSaveEvent:
            if (e.data.save.state_len)
                *e.data.save.state_len = sizeof(u32);
            if (e.data.save.checksum)
                *e.data.save.checksum = 0;
            break;
        case GekkoLoadEvent:
            ASSERT(!has_load);
            has_load   = true;
            load_frame = e.data.load.frame;
            break;
        case GekkoAdvanceEvent:
            if (num_adv < MAX_ROLLBACK_FRAMES + 1)
            {
                ExtractPads(e.data.adv.inputs, adv_pads[num_adv]);
                m_pending_ops.adv_frames[num_adv] = static_cast<u32>(e.data.adv.frame);
                m_pending_ops.adv_rollback[num_adv] = e.data.adv.rolling_back;
                num_adv++;
            }
            break;
        default: break;
        }
    }

    const auto PauseForLocalAdvantage = [](GekkoSession* session) {
      float ahead = gekko_frames_ahead(session);
      if (ahead > 0.6f)
      {
        if (ahead >= static_cast<float>(MAX_ROLLBACK_FRAMES - 1))
        {
          // Hard stall: we're so far ahead that the peer can't keep up.
          // Poll in a tight loop until the gap drops, giving the peer time to catch up.
          auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(32);
          while (gekko_frames_ahead(session) >= static_cast<float>(MAX_ROLLBACK_FRAMES - 1))
          {
            gekko_network_poll(session);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            if (std::chrono::steady_clock::now() >= deadline)
              break;
          }
        }
        else
        {
          // Proportional slowdown: sleep a fraction of a frame scaled to the gap.
          const int sleep_us = static_cast<int>(ahead * ahead * GEKKONET_TIMESYNC_EXTRA_US);
          std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
      }
    };

    if (num_adv == 0)
    {
        PauseForLocalAdvantage(m_session);
        return 0;
    }

    if (rollback_ready && m_rb_initialized)
        rbMgr.SaveFrame(m_system);

    if (has_load)
    {
        const int frames_back =
            num_adv > 0 ? static_cast<int>(m_pending_ops.adv_frames[num_adv - 1]) - load_frame - 1 : 0;
        if (frames_back >= 1 && frames_back <= MAX_ROLLBACK_FRAMES &&
            m_rb_initialized && rbMgr.m_ring_count >= 2 && frames_back < rbMgr.m_ring_count)
        {
            INFO_LOG_FMT(BRAWLBACK, "GekkoNet: rollback frames_back={} load_frame={} last_adv={} resim={}",
                         frames_back, load_frame, m_pending_ops.adv_frames[num_adv - 1], num_adv);
            rbMgr.LoadFrame(m_system, frames_back);
        }
    }

    memcpy(m_pending_ops.adv_pads, adv_pads, sizeof(m_pending_ops.adv_pads));
    m_pending_ops.adv_count = num_adv;

    PauseForLocalAdvantage(m_session);

    {
      const float ahead = gekko_frames_ahead(m_session);
      const u32 color = ahead > 0.5f ? OSD::Color::YELLOW :
                         ahead < -0.5f ? OSD::Color::CYAN : OSD::Color::GREEN;
      OSD::AddTypedMessage(OSD::MessageType::NetPlayPing,
                           fmt::format("Frame adv: {:.1f}", ahead), 1000, color);
    }

    return num_adv;
}

// ---- Matchmaking ----
#define LOCAL_TESTING_GEKKONET
void CEXIBrawlbackGekkoNet::HandleFindOpponent(u8*)
{
#ifdef LOCAL_TESTING_GEKKONET
    std::string ip_file = File::GetExeDirectory() + "/gkk_connect.txt";
    bool is_host = true;
    u16 lport = 7780, rport = 7781;
    if (File::Exists(ip_file))
    {
        std::fstream f;
        File::OpenFStream(f, ip_file, std::ios_base::in);
        std::string line;
        std::getline(f, line);
        if (line == "client") { is_host = false; std::swap(lport, rport); }
        f.close();
    }
    m_is_host = is_host;
    m_local_player_idx = is_host ? 0 : 1;
    m_remote_addr_str = "127.0.0.1:" + std::to_string(rport);
    InitGekkoSession(m_remote_addr_str, lport);
#else
    if (m_matchmaking_thread.joinable())
        m_matchmaking_thread.join();
    m_matchmaking_thread = std::thread(&CEXIBrawlbackGekkoNet::MatchmakingThread, this);
#endif
}

void CEXIBrawlbackGekkoNet::MatchmakingThread()
{
    Common::SetCurrentThreadName("GekkoNet_Matchmaking");
    Matchmaking::MatchSearchSettings settings;
    settings.mode = Matchmaking::OnlinePlayMode::UNRANKED;
    m_matchmaking->FindMatch(settings);
    m_matchmaking->MatchmakeThread();
    if (m_matchmaking->GetMatchmakeState() != Matchmaking::CONNECTION_SUCCESS) return;
    m_is_host          = m_matchmaking->IsHost();
    m_local_player_idx = m_matchmaking->LocalPlayerIndex();
    auto ips   = m_matchmaking->GetRemoteIPAddresses();
    auto ports = m_matchmaking->GetRemotePorts();
    if (!ips.empty())
    {
        m_remote_addr_str = ips[0] + ":" + std::to_string(ports[0]);
        InitGekkoSession(m_remote_addr_str, m_matchmaking->GetLocalPort());
    }
}

void CEXIBrawlbackGekkoNet::HandleStartMatch(u8* payload)
{
    if (!payload) return;
    HLE::Patch(m_system, BRAWL_GAME_LOOP_HOOK_ADDR, "BrawlbackGekkoNetGameLoop");
    HLE::Patch(m_system, BRAWL_GAMEPROC_CALLSITE_ADDR, "BrawlbackGekkoNetGameProcCallsite");
    HLE::Patch(m_system, BRAWL_CLEAR_PAD_EDGE_CALLSITE_ADDR,
               "BrawlbackGekkoNetClearPadEdgeCallsite");
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: patched mainLoopSub gameProc loop hooks");

    m_game_settings = std::make_unique<Match::GameSettings>(
        *reinterpret_cast<Match::GameSettings*>(payload));
    m_num_players = m_game_settings->numPlayers;
    if (m_num_players == 0) m_num_players = 2;
    HandleFindOpponent(nullptr);
    Match::GameSettings remote_settings{};
    if (ExchangeGameSettings(&remote_settings))
        MergeGameSettings(remote_settings);
    else
        m_game_settings->localPlayerIdx = static_cast<u8>(m_local_player_idx);
    QueueGameSettingsResponse();
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: CPU core {}", m_system.GetPowerPC().GetCPUName());
}

void CEXIBrawlbackGekkoNet::HandleEndMatch()
{
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: end match");
    HLE::UnPatch(m_system, "BrawlbackGekkoNetGameLoop");
    HLE::UnPatch(m_system, "BrawlbackGekkoNetGameProcCallsite");
    HLE::UnPatch(m_system, "BrawlbackGekkoNetClearPadEdgeCallsite");
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: unpatched mainLoopSub gameProc loop hooks");

    s_override_active = false;
    m_rb_initialized = false;
    m_current_frame = 0;
    m_pending_ops.Clear();
    m_game_settings.reset();
    DestroyGekkoSession();
}

void CEXIBrawlbackGekkoNet::HandleRegisterExclude(u8* payload)
{
    if (!payload) return;
    u32 addr = Common::swap32(*reinterpret_cast<u32*>(payload));
    u32 sz   = Common::swap32(*reinterpret_cast<u32*>(payload + 4));
    Rollback::RollbackManager::Get().AddExcludeRegion(addr, sz);
}

// ---- GekkoNet session ----

void CEXIBrawlbackGekkoNet::InitGekkoSession(const std::string& remote_addr, unsigned short local_port)
{
    DestroyGekkoSession();
    m_local_port = local_port;
    if (!gekko_create(&m_session, GekkoGameSession)) return;
    GekkoConfig cfg{};
    cfg.num_players             = 2;
    cfg.input_size              = sizeof(BrawlbackPad);
    cfg.state_size              = sizeof(u32);
    cfg.input_prediction_window = static_cast<unsigned char>(MAX_ROLLBACK_FRAMES);
    cfg.max_spectators          = 0;
    cfg.desync_detection        = DEV_DESYNC_MODE;
    cfg.limited_saving          = false;
    gekko_start(m_session, &cfg);
    gekko_net_adapter_set(m_session, gekko_default_adapter(local_port));
    GekkoNetAddress addr{const_cast<char*>(m_remote_addr_str.data()),
                         static_cast<unsigned int>(m_remote_addr_str.size())};
    if (m_local_player_idx == 0)
    {
        m_local_handle = gekko_add_actor(m_session, GekkoLocalPlayer, nullptr);
        m_remote_handle = gekko_add_actor(m_session, GekkoRemotePlayer, &addr);
    }
    else
    {
        m_remote_handle = gekko_add_actor(m_session, GekkoRemotePlayer, &addr);
        m_local_handle = gekko_add_actor(m_session, GekkoLocalPlayer, nullptr);
    }
    gekko_set_local_delay(m_session, m_local_handle, FRAME_DELAY);
    m_connect_wait_ticks = 0;
    INFO_LOG_FMT(BRAWLBACK,
                 "GekkoNet session init: init port={} remote={} local_player={} local_handle={} remote_handle={}",
                 local_port, remote_addr, m_local_player_idx, m_local_handle, m_remote_handle);
}

// TODO: gekko uses asio by default, but I know how to use enet a little better, so i opted for that here.
// In the future, we should make an enet "adapter" for gekkonet to use instead of asio - which could simplify this code a bit (use the same stuff the gekko adapter uses)
bool CEXIBrawlbackGekkoNet::ExchangeGameSettings(Match::GameSettings* remote_settings) const
{
    if (!remote_settings || !m_game_settings || !m_enet_initialized || m_remote_addr_str.empty() ||
        m_local_port == 0)
        return false;

    constexpr u32 SETTINGS_MAGIC = 0x474B4753; // GKGS
    struct SettingsPacket
    {
        u32 magic;
        Match::GameSettings settings;
    };

    const auto colon = m_remote_addr_str.rfind(':');
    if (colon == std::string::npos)
        return false;

    const std::string remote_ip = m_remote_addr_str.substr(0, colon);
    const auto remote_port = static_cast<unsigned short>(std::stoi(m_remote_addr_str.substr(colon + 1)));
    const auto local_settings_port = static_cast<unsigned short>(m_local_port + 100);
    const auto remote_settings_port = static_cast<unsigned short>(remote_port + 100);
    const SettingsPacket outgoing{SETTINGS_MAGIC, *m_game_settings};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(80);

    if (m_local_player_idx == 0)
    {
        ENetAddress local_addr{};
        local_addr.host = ENET_HOST_ANY;
        local_addr.port = local_settings_port;
        Common::ENet::ENetHostPtr host(enet_host_create(&local_addr, 1, 1, 0, 0));
        if (!host)
            return false;

        while (std::chrono::steady_clock::now() < deadline)
        {
            ENetEvent event{};
            while (enet_host_service(host.get(), &event, 10) > 0)
            {
                if (event.type == ENET_EVENT_TYPE_RECEIVE)
                {
                    const auto* incoming = static_cast<const SettingsPacket*>(
                        static_cast<const void*>(event.packet->data));
                    if (event.packet->dataLength == sizeof(SettingsPacket) &&
                        incoming->magic == SETTINGS_MAGIC)
                    {
                        *remote_settings = incoming->settings;
                        ENetPacket* packet = enet_packet_create(&outgoing, sizeof(outgoing),
                                                                ENET_PACKET_FLAG_RELIABLE);
                        enet_peer_send(event.peer, 0, packet);
                        enet_host_flush(host.get());
                        enet_packet_destroy(event.packet);
                        return true;
                    }
                    enet_packet_destroy(event.packet);
                }
            }
        }

        WARN_LOG_FMT(BRAWLBACK, "GekkoNet: host settings exchange timed out");
        return false;
    }

    ENetAddress remote_addr{};
    if (enet_address_set_host(&remote_addr, remote_ip.c_str()) != 0)
        return false;
    remote_addr.port = remote_settings_port;

    Common::ENet::ENetHostPtr host(enet_host_create(nullptr, 1, 1, 0, 0));
    if (!host)
        return false;

    ENetPeer* peer = enet_host_connect(host.get(), &remote_addr, 1, 0);
    if (!peer)
        return false;

    bool sent_settings = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        ENetEvent event{};
        while (enet_host_service(host.get(), &event, 10) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT && !sent_settings)
            {
                ENetPacket* packet =
                    enet_packet_create(&outgoing, sizeof(outgoing), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(peer, 0, packet);
                enet_host_flush(host.get());
                sent_settings = true;
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                const auto* incoming =
                    static_cast<const SettingsPacket*>(static_cast<const void*>(event.packet->data));
                if (event.packet->dataLength == sizeof(SettingsPacket) &&
                    incoming->magic == SETTINGS_MAGIC)
                {
                    *remote_settings = incoming->settings;
                    enet_packet_destroy(event.packet);
                    return true;
                }
                enet_packet_destroy(event.packet);
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                return false;
            }
        }
    }

    WARN_LOG_FMT(BRAWLBACK, "GekkoNet: client settings exchange timed out");
    return false;
}

void CEXIBrawlbackGekkoNet::MergeGameSettings(const Match::GameSettings& remote_settings)
{
    if (!m_game_settings)
        return;

    const Match::GameSettings local_settings = *m_game_settings;
    const bool is_host = m_local_player_idx == 0;
    const Match::GameSettings& host_settings = is_host ? local_settings : remote_settings;
    const Match::GameSettings& client_settings = is_host ? remote_settings : local_settings;

    m_game_settings->localPlayerIdx = static_cast<u8>(m_local_player_idx);
    m_game_settings->numPlayers = 2;
    m_game_settings->randomSeed = host_settings.randomSeed;
    m_game_settings->stageID = host_settings.stageID;
    m_game_settings->playerSettings[0] = host_settings.playerSettings[0];
    m_game_settings->playerSettings[1] = client_settings.playerSettings[0];
    m_game_settings->playerSettings[0].playerType =
        is_host ? Match::PlayerType::PLAYERTYPE_LOCAL : Match::PlayerType::PLAYERTYPE_REMOTE;
    m_game_settings->playerSettings[1].playerType =
        is_host ? Match::PlayerType::PLAYERTYPE_REMOTE : Match::PlayerType::PLAYERTYPE_LOCAL;
}

void CEXIBrawlbackGekkoNet::QueueGameSettingsResponse()
{
    if (!m_game_settings)
        return;

    auto bytes = Mem::structToByteVector(m_game_settings.get());
    m_read_queue.clear();
    m_read_queue.push_back(static_cast<u8>(GKK_SETUP_PLAYERS));
    m_read_queue.insert(m_read_queue.end(), bytes.begin(), bytes.end());
}

void CEXIBrawlbackGekkoNet::DestroyGekkoSession()
{
    if (m_session)
    {
        gekko_destroy(&m_session);
        m_session = nullptr;
        m_session_started = false;
        m_seen_match_frame_zero = false;
        m_connect_wait_ticks = 0;
    }
    m_local_port = 0;
    gekko_default_adapter_destroy();
}

// ---- Helpers ----

void CEXIBrawlbackGekkoNet::ExtractPads(const unsigned char* inputs,
                                         BrawlbackPad out[MAX_NUM_PLAYERS]) const
{
    memset(out, 0, sizeof(BrawlbackPad) * MAX_NUM_PLAYERS);
    for (int i = 0; i < m_num_players; i++)
        memcpy(&out[i], inputs + i * sizeof(BrawlbackPad), sizeof(BrawlbackPad));
}

UserInfo CEXIBrawlbackGekkoNet::GetUserInfo() const
{
    UserInfo info;
#ifdef _WIN32
    std::string path = File::GetExeDirectory() + "/lylat.json";
#else
    std::string path = File::GetUserPath(D_USER_IDX) + "lylat.json";
#endif
    std::string data;
    if (!File::ReadFileToString(path, data)) return info;
    json j           = json::parse(data);
    info.uid         = j["uid"].get<std::string>();
    info.playKey     = j["playKey"].get<std::string>();
    info.connectCode = j["connectCode"].get<std::string>();
    info.displayName = "GekkoNet Player";
    return info;
}
