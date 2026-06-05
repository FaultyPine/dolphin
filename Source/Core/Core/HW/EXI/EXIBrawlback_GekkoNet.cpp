
#include "EXIBrawlback_GekkoNet.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/RollbackManager.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/OnScreenDisplay.h"

#include "Core/Brawlback/include/json.hpp"
using json = nlohmann::json;

#include <chrono>
#include <cstring>
#include <thread>

// ---- Construction / Destruction ----

CEXIBrawlbackGekkoNet::CEXIBrawlbackGekkoNet(Core::System& system) : IEXIDevice(system)
{
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet EXI device created");
    m_matchmaking = std::make_unique<Matchmaking>(GetUserInfo());
}

CEXIBrawlbackGekkoNet::~CEXIBrawlbackGekkoNet()
{
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

// ---- Input injection ----
// Dual path: SI override (catches PADRead if it fires during SingleStep)
//            + direct memory write to gfPadSystem raw buffer (guaranteed path).
//
// Input chain in Brawl:
//   PADRead (pad thread, alarm-driven) → SI device → writes to pads[+0x40]
//   gameProc → ipSwitch::update → updateGame → copies pads[+0x40] → processed[+0x444]
//                                → getGamePadStatus → reads processed[+0x444]
//
// PADRead runs on a separate PPC thread. During RunGameProc (SingleStep),
// it may not fire, so the raw buffer would be stale. We write there directly.

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

// ---- PPC function calling ----

void CEXIBrawlbackGekkoNet::RunPPCFunction(u32 addr, u32 r3, u32 r4)
{
    auto& ppc   = m_system.GetPowerPC();
    auto& state = ppc.GetPPCState();

    // Save volatile PPC state
    u32 saved_pc  = state.pc;
    u32 saved_npc = state.npc;
    u32 saved_lr  = state.spr[SPR_LR];
    u32 saved_ctr = state.spr[SPR_CTR];
    auto saved_cr = state.cr;
    u8  saved_xer_ca    = state.xer_ca;
    u8  saved_xer_so_ov = state.xer_so_ov;
    u32 saved_gpr[32];
    memcpy(saved_gpr, state.gpr, sizeof(saved_gpr));
    PowerPC::PairedSingle saved_ps[14];
    memcpy(saved_ps, state.ps, sizeof(PowerPC::PairedSingle) * 14);

    state.gpr[3] = r3;
    state.gpr[4] = r4;
    state.pc     = addr;
    state.npc    = addr + 4;
    state.spr[SPR_LR] = 0;

    while (state.pc != 0)
        ppc.SingleStep();

    state.pc  = saved_pc;
    state.npc = saved_npc;
    state.spr[SPR_LR]  = saved_lr;
    state.spr[SPR_CTR] = saved_ctr;
    state.cr  = saved_cr;
    state.xer_ca    = saved_xer_ca;
    state.xer_so_ov = saved_xer_so_ov;
    memcpy(state.gpr, saved_gpr, sizeof(saved_gpr));
    memcpy(state.ps, saved_ps, sizeof(PowerPC::PairedSingle) * 14);
}

void CEXIBrawlbackGekkoNet::RunGameProc(int resim_index)
{
    u32 gf_app = m_system.GetMemory().Read_U32(BRAWL_GF_APPLICATION_PTR);
    RunPPCFunction(BRAWL_GAMEPROC_ADDR, gf_app, static_cast<u32>(resim_index));
}

// ---- Core per-frame handler ----
// Called once per mainLoopSub iteration from the game's DMAWrite hook.
// LoadFrame does NOT restore PPC state → resim happens inline, no re-entry.

void CEXIBrawlbackGekkoNet::HandleFrame(u8* payload)
{
    if (!payload) return;

    auto* p = reinterpret_cast<GKKFramePayload*>(payload);
    int frame = static_cast<int>(Common::swap32(p->frame));
    m_current_frame = frame;
    auto& rbMgr = Rollback::RollbackManager::Get();

    if (frame == GAME_START_FRAME && !m_rb_initialized)
    {
        rbMgr.Init(m_system);
        m_rb_initialized = true;
    }

    if (!m_session || !m_session_started)
    {
        return;
    }

    // Feed GekkoNet
    gekko_add_local_input(m_session, m_local_handle, &p->pad);
    gekko_network_poll(m_session);

    int sc = 0;
    auto** se = gekko_session_events(m_session, &sc);
    for (int i = 0; i < sc; i++)
    {
        if (se[i]->type == GekkoSessionStarted) m_session_started = true;
        else if (se[i]->type == GekkoPlayerDisconnected) { return; }
        else if (se[i]->type == GekkoDesyncDetected)
            WARN_LOG_FMT(BRAWLBACK, "GekkoNet: desync frame {}", se[i]->data.desynced.frame);
    }
    if (!m_session_started) { return; }

    // Drive session
    int gc = 0;
    auto** ge = gekko_update_session(m_session, &gc);

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
            *e.data.save.state_len = sizeof(u32);
            *e.data.save.checksum  = 0;
            break;
        case GekkoLoadEvent:
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
        return;
    }

    if (m_rb_initialized)
        rbMgr.SaveFrame(m_system);

    // Rollback: LoadFrame restores RAM, NOT PPC. Resim happens right here.
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

    // Execute all advance frames
    for (int i = 0; i < num_adv; i++)
    {
        if (m_rb_initialized && i > 0)
            rbMgr.SaveFrame(m_system);
        InjectPads(adv_pads[i]);
        RunGameProc(i);
    }

    s_override_active = false;

    float ahead = gekko_frames_ahead(m_session);
    if (ahead > 0.5f)
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(ahead * 275.0f)));
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
    m_game_settings = std::make_unique<Match::GameSettings>(
        *reinterpret_cast<Match::GameSettings*>(payload));
    m_num_players = m_game_settings->numPlayers;
    if (m_num_players == 0) m_num_players = 2;
    m_game_settings->localPlayerIdx = static_cast<u8>(m_local_player_idx);
    auto bytes = Mem::structToByteVector(m_game_settings.get());
    m_read_queue.clear();
    m_read_queue.push_back(static_cast<u8>(GKK_SETUP_PLAYERS));
    m_read_queue.insert(m_read_queue.end(), bytes.begin(), bytes.end());
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
    m_local_handle = gekko_add_actor(m_session, GekkoLocalPlayer, nullptr);
    gekko_set_local_delay(m_session, m_local_handle, FRAME_DELAY);
    GekkoNetAddress addr{const_cast<char*>(m_remote_addr_str.data()),
                         static_cast<unsigned int>(m_remote_addr_str.size())};
    m_remote_handle = gekko_add_actor(m_session, GekkoRemotePlayer, &addr);
    INFO_LOG_FMT(BRAWLBACK, "GekkoNet: init port={} remote={}", local_port, remote_addr);
}

void CEXIBrawlbackGekkoNet::DestroyGekkoSession()
{
    if (m_session) { gekko_destroy(&m_session); m_session = nullptr; m_session_started = false; }
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
