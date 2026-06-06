
#include "EXIBrawlback_GekkoNet.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/RollbackManager.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/OnScreenDisplay.h"
#include <Core/Rollback/Perf.h>

#include "Core/Brawlback/include/json.hpp"
using json = nlohmann::json;

#include <chrono>
#include <cstring>
#include <thread>

// ---- Construction / Destruction ----

CEXIBrawlbackGekkoNet::CEXIBrawlbackGekkoNet(Core::System& system) : IEXIDevice(system)
{
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet EXI device created");
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
    s_override_active = false;
    DestroyGekkoSession();
    if (m_matchmaking_thread.joinable())
        m_matchmaking_thread.join();
    Rollback::RollbackManager::Get().Shutdown();
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
    case GKK_CMD_FRAME:       HandleFrame(payload);           break;
    case GKK_FIND_OPPONENT:   HandleFindOpponent(payload);    break;
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
    if (++m_loop_hook_ticks <= 10 || (m_loop_hook_ticks % 60) == 0)
    {
        INFO_LOG_FMT(BRAWLBACK,
                     "GekkoNet: loop hook frame={} adv={} session={} r23={:08x} r24_before={} r19_before={}",
                     m_current_frame, adv_count, m_session_started, ppc_state.gpr[23], ppc_state.gpr[24],
                     ppc_state.gpr[19]);
    }
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
    if (++m_callsite_hook_ticks <= 10 || (m_callsite_hook_ticks % 60) == 0)
    {
        INFO_LOG_FMT(BRAWLBACK,
                     "GekkoNet: gameProc callsite hit index={} count={} frame={} r23={:08x} lr={:08x}",
                     resim_index, m_pending_adv_count, m_current_frame, ppc_state.gpr[23],
                     ppc_state.spr[SPR_LR]);
    }

    if (resim_index >= 0 && resim_index < m_pending_adv_count)
    {
        if (m_rb_initialized && resim_index > 0)
            Rollback::RollbackManager::Get().SaveFrame(m_system);
        InjectPads(m_pending_adv_pads[resim_index]);
    }
    else if (m_pending_adv_count > 0)
    {
        WARN_LOG_FMT(BRAWLBACK, "GekkoNet: gameProc callsite without pending input index={} count={}",
                     resim_index, m_pending_adv_count);
    }

    ppc_state.gpr[3] = ppc_state.gpr[23];  // original 0x80017350: or r3, r23, r23
    ppc_state.npc = BRAWL_GAMEPROC_CALLSITE_NEXT_ADDR;
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
        s.stickX       = static_cast<u8>(static_cast<int>(pads[i].stickX) + 128);
        s.stickY       = static_cast<u8>(static_cast<int>(pads[i].stickY) + 128);
        s.substickX    = static_cast<u8>(static_cast<int>(pads[i].cStickX) + 128);
        s.substickY    = static_cast<u8>(static_cast<int>(pads[i].cStickY) + 128);
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
    auto& mem = m_system.GetMemory();
    GKKFramePayload payload{};
    payload.frame = Rollback::ReadBrawlMatchFrameCounter(mem.GetEXRAM(), mem.GetExRamSize());

    const u32 pad_base =
        BRAWL_PAD_RAW_BASE + static_cast<u32>(m_local_player_idx) * BRAWL_PAD_STRIDE;
    payload.pad.buttons = mem.Read_U16(pad_base + PAD_OFF_BUTTONS);
    payload.pad.stickX = static_cast<char>(mem.Read_U8(pad_base + PAD_OFF_STICKS + 0));
    payload.pad.stickY = static_cast<char>(mem.Read_U8(pad_base + PAD_OFF_STICKS + 1));
    payload.pad.cStickX = static_cast<char>(mem.Read_U8(pad_base + PAD_OFF_STICKS + 2));
    payload.pad.cStickY = static_cast<char>(mem.Read_U8(pad_base + PAD_OFF_STICKS + 3));
    payload.pad.LTrigger = static_cast<char>(mem.Read_U8(pad_base + PAD_OFF_STICKS + 4));
    payload.pad.RTrigger = static_cast<char>(mem.Read_U8(pad_base + PAD_OFF_STICKS + 5));
    return payload;
}

// ---- Core per-frame handler ----
// Called once per mainLoopSub iteration from the HLE loop hook.
// LoadFrame does NOT restore PPC state → resim happens inline, no re-entry.

int CEXIBrawlbackGekkoNet::HandleFrame(u8* payload)
{
    m_pending_adv_count = 0;
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

    const bool gameplay_frame = frame > 0;
    if (m_session_started && gameplay_frame)
        gekko_add_local_input(m_session, m_local_handle, &p->pad);

    int gc = 0;
    auto** ge = gekko_update_session(m_session, &gc);

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
            return 0;
        case GekkoDesyncDetected:
            WARN_LOG_FMT(BRAWLBACK, "GekkoNet: desync frame {}", se[i]->data.desynced.frame);
            break;
        default:
            break;
        }
    }
    if (!m_session_started)
    {
        if (++m_connect_wait_ticks % 120 == 0)
            INFO_LOG_FMT(BRAWLBACK, "GekkoNet: waiting for session start remote={} local_handle={} remote_handle={}",
                         m_remote_addr_str, m_local_handle, m_remote_handle);
        return gameplay_frame ? 0 : 1;
    }

    if (!gameplay_frame)
    {
        if ((m_loop_hook_ticks % 120) == 0)
            INFO_LOG_FMT(BRAWLBACK, "GekkoNet: pass-through pre-gameplay frame={}", frame);
        return 1;
    }

    bool has_load   = false;
    int  load_frame = 0;
    int  num_adv    = 0;
    BrawlbackPad adv_pads[MAX_ROLLBACK_FRAMES + 1][MAX_NUM_PLAYERS] = {};

    for (int i = 0; i < gc; i++)
    {
        auto& e = *ge[i];
        switch (e.type)
        {
        case GekkoSaveEvent:
            // we SaveFrame every frame anyway so this event isn't really used rn
            *e.data.save.state_len = sizeof(u32);
            *e.data.save.checksum  = 0;
            break;
        case GekkoLoadEvent:
            ASSERT(!has_load);
            has_load   = true;
            load_frame = e.data.load.frame;
            break;
        case GekkoAdvanceEvent:
            if (num_adv < MAX_ROLLBACK_FRAMES + 1)
                ExtractPads(e.data.adv.inputs, adv_pads[num_adv++]);
            break;
        default: break;
        }
    }

    if (num_adv == 0)
    {
        float ahead = gekko_frames_ahead(m_session);
        if (ahead > 0.5f)
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(ahead * 275.0f)));
        return 0;
    }

    if (m_rb_initialized)
        rbMgr.SaveFrame(m_system);

    if (has_load)
    {
        int frames_back = m_current_frame - load_frame - 1;
        if (frames_back >= 1 && frames_back <= MAX_ROLLBACK_FRAMES &&
            m_rb_initialized && rbMgr.m_ring_count >= 2 && frames_back < rbMgr.m_ring_count)
        {
            INFO_LOG_FMT(BRAWLBACK, "GekkoNet: rollback frames_back={} resim={}", frames_back, num_adv);
            rbMgr.LoadFrame(m_system, frames_back);
        }
    }

    memcpy(m_pending_adv_pads, adv_pads, sizeof(m_pending_adv_pads));
    m_pending_adv_count = num_adv;

    float ahead = gekko_frames_ahead(m_session);
    if (ahead > 0.6f)
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(ahead * 275.0f))); // TODO: Why 275?
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
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: patched mainLoopSub gameProc loop hooks");

    m_game_settings = std::make_unique<Match::GameSettings>(
        *reinterpret_cast<Match::GameSettings*>(payload));
    m_num_players = m_game_settings->numPlayers;
    if (m_num_players == 0) m_num_players = 2;
    m_game_settings->localPlayerIdx = static_cast<u8>(m_local_player_idx);
    auto bytes = Mem::structToByteVector(m_game_settings.get());
    m_read_queue.clear();
    m_read_queue.push_back(static_cast<u8>(GKK_SETUP_PLAYERS));
    m_read_queue.insert(m_read_queue.end(), bytes.begin(), bytes.end());
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: CPU core {}", m_system.GetPowerPC().GetCPUName());
}

void CEXIBrawlbackGekkoNet::HandleEndMatch()
{
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: end match");
    HLE::UnPatch(m_system, "BrawlbackGekkoNetGameLoop");
    HLE::UnPatch(m_system, "BrawlbackGekkoNetGameProcCallsite");
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: unpatched mainLoopSub gameProc loop hooks");

    s_override_active = false;
    m_rb_initialized = false;
    m_current_frame = 0;
    m_pending_adv_count = 0;
    m_loop_hook_ticks = 0;
    m_callsite_hook_ticks = 0;
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
    if (!gekko_create(&m_session, GekkoGameSession)) return;
    GekkoConfig cfg{};
    cfg.num_players             = 2;
    cfg.input_size              = sizeof(BrawlbackPad);
    cfg.state_size              = sizeof(u32);
    cfg.input_prediction_window = static_cast<unsigned char>(MAX_ROLLBACK_FRAMES);
    cfg.max_spectators          = 0;
    cfg.desync_detection        = false;
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
                 "GekkoNet: init port={} remote={} local_player={} local_handle={} remote_handle={}",
                 local_port, remote_addr, m_local_player_idx, m_local_handle, m_remote_handle);
}

void CEXIBrawlbackGekkoNet::DestroyGekkoSession()
{
    if (m_session)
    {
        gekko_destroy(&m_session);
        m_session = nullptr;
        m_session_started = false;
        m_connect_wait_ticks = 0;
    }
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
