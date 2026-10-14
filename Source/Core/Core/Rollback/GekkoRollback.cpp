// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GekkoRollback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_M_X86_64) || defined(__x86_64__)
#include <xmmintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include <arm64intr.h>
#endif

#include <gekkonet.h>
#include <fmt/format.h>
#include <xxh3.h>

#include "Common/Config/Config.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/FPURoundMode.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DSP.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/DirtyPages.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"

namespace Rollback
{
namespace
{
// All physical controller buttons plus the adapter's synchronized calibration event.
constexpr u16 PAD_WIRE_BUTTONS = 0x1F7F | PAD_GET_ORIGIN;
constexpr int MAX_PORTS = 4;
// Gekko exposes up to 10 rollback frames in the lobby. Keep two additional snapshots so the
// current and confirmed boundary states cannot alias the oldest rollback state in the ring.
constexpr int RING_SNAPSHOT_SLOTS = 12;
constexpr int WAIT_SLEEP_US = 100;
constexpr auto SIMULATED_P2_LATENCY = std::chrono::milliseconds(40);
constexpr u64 STRESS_INPUT_PERIOD_FRAMES = 3;
constexpr std::string_view SIMULATED_P1_ADDRESS = "in-process-p1";
constexpr std::string_view SIMULATED_P2_ADDRESS = "in-process-p2";
constexpr u32 BRAWL_FRAME_HOOK_ADDR = 0x80017504;
constexpr u32 BRAWL_EXPECTED_OPCODE = 0x90170100;  // stw r0, 0x100(r23)
constexpr std::array<u8, 4> RAM_DIAGNOSTIC_MAGIC{'R', 'M', 'D', 'G'};
constexpr u8 RAM_DIAGNOSTIC_VERSION = 4;
constexpr u8 RAM_DIAGNOSTIC_DIGEST = 1;
constexpr u8 RAM_DIAGNOSTIC_MISMATCH = 2;
constexpr std::size_t RAM_DIAGNOSTIC_PACKET_SIZE =
    112 + State::ROLLBACK_STATE_SECTION_COUNT * sizeof(u64);
constexpr std::array<std::string_view, State::ROLLBACK_STATE_SECTION_COUNT>
    ROLLBACK_STATE_SECTION_NAMES{"Movie", "VideoBackend", "CoreTiming", "HW", "PowerPC",
                                 "Wiimote", "Gecko"};

bool HandleRamDiagnosticDatagram(const Common::ENet::RollbackDatagram& datagram);

constexpr std::string_view FrameBoundaryName(FrameBoundary boundary)
{
  switch (boundary)
  {
  case FrameBoundary::BrawlHook:
    return "brawl_hook";
  case FrameBoundary::VIBeginField:
    return "vi_begin_field";
  case FrameBoundary::VIEndField:
    return "vi_end_field";
  case FrameBoundary::VINewField:
    return "vi_new_field";
  }
  return "unknown";
}

// Host floating point scope to protect guest FPU rounding modes
u64 HostFloatControl()
{
#if defined(_M_X86_64) || defined(__x86_64__)
  return _mm_getcsr();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
  return _ReadStatusReg(ARM64_FPCR);
#elif defined(__aarch64__)
  u64 fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
#else
  return 0;
#endif
}

class HostFloatScope
{
public:
  explicit HostFloatScope(PowerPC::PowerPCState& ppc_state) : m_ppc_state(ppc_state)
  {
    Common::FPU::LoadDefaultSIMDState();
  }
  ~HostFloatScope() { PowerPC::RoundingModeUpdated(m_ppc_state); }
  HostFloatScope(const HostFloatScope&) = delete;
  HostFloatScope& operator=(const HostFloatScope&) = delete;

private:
  PowerPC::PowerPCState& m_ppc_state;
};

struct QueuedEvent
{
  GekkoGameEventType type = GekkoEmptyGameEvent;
  int frame = -1;
  std::vector<unsigned char> inputs;
  unsigned int* checksum = nullptr;
  unsigned int* state_len = nullptr;
  bool rolling_back = false;
  bool running_ahead = false;
};

struct SimulatedPacket
{
  std::chrono::steady_clock::time_point delivery_time;
  u64 delivery_frame = 0;
  bool frame_delayed = false;
  std::vector<char> data;
};

struct SampledLocalInput
{
  s64 frame = 0;
  WirePad input{};
};

std::deque<SimulatedPacket> s_packets_to_main;
std::deque<SimulatedPacket> s_packets_to_fake;
std::vector<GekkoNetResult*> s_main_results;
std::vector<GekkoNetResult*> s_fake_results;
std::vector<GekkoNetResult*> s_shared_results;
u64 s_simulated_link_frame = 0;
u64 s_last_stress_peer_update_frame = 0;
u64 s_stress_packet_delay_frames = 0;
bool s_stress_mode = false;
bool s_stress_frame_delay_active = false;

bool ParsePeerEndpoint(std::string_view endpoint, ENetAddress* address)
{
  const size_t colon = endpoint.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == endpoint.size())
    return false;

  unsigned int port = 0;
  const char* port_begin = endpoint.data() + colon + 1;
  const char* port_end = endpoint.data() + endpoint.size();
  const auto [end, error] = std::from_chars(port_begin, port_end, port);
  if (error != std::errc{} || end != port_end || port == 0 || port > 65535)
    return false;

  const std::string host(endpoint.substr(0, colon));
  address->port = static_cast<u16>(port);
  return enet_address_set_host(address, host.c_str()) == 0;
}

void SharedAdapterSend(GekkoNetAddress* address, const char* data, int length)
{
  if (!address || address->size != sizeof(ENetAddress) || length < 0)
    return;

  ENetAddress endpoint{};
  std::memcpy(&endpoint, address->data, sizeof(endpoint));
  if (!Common::ENet::SendRollbackDatagram(endpoint, data, static_cast<size_t>(length)))
    WARN_LOG_FMT(CORE, "GekkoNet: failed to send shared-socket datagram");
}

GekkoNetResult** SharedAdapterReceive(int* length)
{
  s_shared_results.clear();
  for (Common::ENet::RollbackDatagram& datagram : Common::ENet::DrainRollbackDatagrams())
  {
    if (HandleRamDiagnosticDatagram(datagram))
      continue;
    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = sizeof(ENetAddress);
    result->addr.data = std::malloc(sizeof(ENetAddress));
    std::memcpy(result->addr.data, &datagram.address, sizeof(ENetAddress));
    result->data_len = static_cast<unsigned int>(datagram.payload.size());
    result->data = std::malloc(datagram.payload.size());
    std::memcpy(result->data, datagram.payload.data(), datagram.payload.size());
    s_shared_results.push_back(result);
  }
  *length = static_cast<int>(s_shared_results.size());
  return s_shared_results.data();
}

void SharedAdapterFree(void* data)
{
  std::free(data);
}

GekkoNetAdapter s_shared_adapter{SharedAdapterSend, SharedAdapterReceive, SharedAdapterFree};

void QueueSimulatedPacket(std::deque<SimulatedPacket>* destination, const char* data, int length)
{
  SimulatedPacket packet;
  packet.data.assign(data, data + length);
  if (s_stress_mode && s_stress_frame_delay_active)
  {
    packet.delivery_frame = s_simulated_link_frame + s_stress_packet_delay_frames;
    packet.frame_delayed = true;
  }
  else
  {
    // Stress mode keeps handshake traffic immediate. Frame-delayed delivery begins only after
    // Gekko reports that the session has started, avoiding a frame-zero synchronization deadlock.
    packet.delivery_time = std::chrono::steady_clock::now() +
                           (s_stress_mode ? std::chrono::milliseconds(0) : SIMULATED_P2_LATENCY);
  }
  destination->push_back(std::move(packet));
}

void MainAdapterSend(GekkoNetAddress*, const char* data, int length)
{
  QueueSimulatedPacket(&s_packets_to_fake, data, length);
}

void FakeAdapterSend(GekkoNetAddress*, const char* data, int length)
{
  QueueSimulatedPacket(&s_packets_to_main, data, length);
}

GekkoNetResult** ReceiveSimulatedPackets(std::deque<SimulatedPacket>* inbox,
                                         std::vector<GekkoNetResult*>* results,
                                         std::string_view sender, int* length)
{
  results->clear();
  const auto now = std::chrono::steady_clock::now();
  while (!inbox->empty())
  {
    const SimulatedPacket& front = inbox->front();
    const bool ready = front.frame_delayed ? s_simulated_link_frame >= front.delivery_frame :
                                             front.delivery_time <= now;
    if (!ready)
      break;
    SimulatedPacket packet = std::move(inbox->front());
    inbox->pop_front();

    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = static_cast<unsigned int>(sender.size());
    result->addr.data = std::malloc(sender.size());
    std::memcpy(result->addr.data, sender.data(), sender.size());
    result->data_len = static_cast<unsigned int>(packet.data.size());
    result->data = std::malloc(packet.data.size());
    std::memcpy(result->data, packet.data.data(), packet.data.size());
    results->push_back(result);
  }
  *length = static_cast<int>(results->size());
  return results->data();
}

GekkoNetResult** MainAdapterReceive(int* length)
{
  return ReceiveSimulatedPackets(&s_packets_to_main, &s_main_results, SIMULATED_P2_ADDRESS, length);
}

GekkoNetResult** FakeAdapterReceive(int* length)
{
  return ReceiveSimulatedPackets(&s_packets_to_fake, &s_fake_results, SIMULATED_P1_ADDRESS, length);
}

void SimulatedAdapterFree(void* data)
{
  std::free(data);
}

GekkoNetAdapter s_main_simulated_adapter{MainAdapterSend, MainAdapterReceive, SimulatedAdapterFree};
GekkoNetAdapter s_fake_simulated_adapter{FakeAdapterSend, FakeAdapterReceive, SimulatedAdapterFree};

void ResetSimulatedLink()
{
  s_packets_to_main.clear();
  s_packets_to_fake.clear();
  s_main_results.clear();
  s_fake_results.clear();
  s_simulated_link_frame = 0;
  s_last_stress_peer_update_frame = 0;
  s_stress_packet_delay_frames = 0;
  s_stress_mode = false;
  s_stress_frame_delay_active = false;
}

struct RamDigest
{
  int frame = -1;
  u64 root = 0;
  u64 mem1 = 0;
  u64 mem2 = 0;
  u64 aram = 0;
  u64 fake_vmem = 0;
  u64 l1 = 0;
  u64 inputs = 0;
  u64 state = 0;
  u64 core_ticks = 0;
  u64 timebase = 0;
  u64 fake_tb_start_value = 0;
  u64 fake_tb_start_ticks = 0;
  u32 decrementer = 0;
  std::array<u64, State::ROLLBACK_STATE_SECTION_COUNT> state_sections{};
  int sends = 0;
};

struct DiagnosticEvent
{
  GekkoGameEventType type = GekkoEmptyGameEvent;
  int frame = -1;
  bool rolling_back = false;
  bool running_ahead = false;
  u64 inputs = 0;
  u64 ticks = 0;
  u64 nand_mark = 0;
};

struct GekkoManager
{
  std::recursive_mutex mutex;
  GekkoSession* session = nullptr;
  GekkoSession* simulated_peer_session = nullptr;
  std::unique_ptr<SnapshotRing> ring;

  int players = 2;
  int local_player = 1;
  int local_handle = -1;
  int simulated_peer_p2_handle = -1;
  int remote_handle = -1;
  bool native_adapter_active = false;
  bool local_uses_gc_adapter = false;
  bool debug_p2_cstick = false;
  bool simulate_remote_p2 = false;
  bool stress_test = false;
  bool tracked_bitmap_clear = false;
  bool full_scan_benchmark = false;
  bool compare_confirmed_ram = false;
  bool ram_diagnostic_dumped = false;
  FrameBoundary frame_boundary = FrameBoundary::VINewField;
  std::atomic<bool> vi_boundary_pending{false};
  std::atomic<bool> precise_vi_tail{false};
  int configured_local_delay = 0;
  u32 session_id = 0;
  u64 local_input_frame = 0;
  std::vector<int> player_handles;
  std::vector<ENetAddress> remote_endpoints;
  std::deque<QueuedEvent> pending_events;
  std::deque<SampledLocalInput> sampled_local_inputs;
  std::array<GCPadStatus, MAX_PORTS> latched_pads{};
  std::atomic<bool> active{false};
  std::atomic<bool> stop_requested{false};

  std::map<int, RamDigest> ram_digests;
  std::map<std::pair<int, int>, RamDigest> remote_ram_digests;
  std::optional<int> first_ram_mismatch_frame;
  std::optional<int> ram_mismatch_details_frame;
  u64 last_advance_input_hash = 0;
  bool last_advance_has_start = false;
  bool last_sampled_start = false;
  std::deque<DiagnosticEvent> diagnostic_events;
  bool restore_mismatch_reported = false;
  u64 perf_restore_checks = 0;
  u64 perf_restore_mismatches = 0;

  // Pacing
  double speed_scale = 1.0;
  double target_scale = 1.0;
  int timesync_counter = 0;
  std::optional<std::pair<s64, TimePoint>> throttle_reference_before_load;
  Common::PrecisionTimer wait_timer;

  // Rolling diagnostics. These distinguish emulator snapshot cost from time spent waiting for
  // GekkoNet to authorize the next frame.
  std::chrono::steady_clock::time_point perf_window_start{};
  DirtyPages::Counters perf_dirty_pages_start{};
  u64 perf_real_frames = 0;
  u64 perf_replay_frames = 0;
  u64 perf_save_count = 0;
  u64 perf_load_count = 0;
  u64 perf_pump_count = 0;
  double perf_save_ms = 0.0;
  double perf_save_max_ms = 0.0;
  double perf_load_ms = 0.0;
  double perf_load_max_ms = 0.0;
  double perf_pump_ms = 0.0;
  double perf_pump_max_ms = 0.0;
  SnapshotPhaseTimings perf_save_phases{};
  SnapshotPhaseTimings perf_load_phases{};
  std::optional<WirePad> last_local_wire;
  u64 perf_local_input_changes = 0;
  std::array<u64, sizeof(WirePad)> perf_local_byte_changes{};
  std::optional<WirePad> last_applied_local_wire;
  u64 perf_delay_checks = 0;
  u64 perf_delay_matches = 0;
  u64 perf_delay_mismatches = 0;
  u64 perf_delay_missing = 0;
  u64 perf_delay_transitions = 0;
  u64 perf_delay_transition_frames = 0;
  u64 perf_delay_transition_min = 0;
  u64 perf_delay_transition_max = 0;
  std::optional<std::chrono::steady_clock::time_point> frame_execution_start;
  bool frame_execution_resim = false;
  int frame_execution_gekko_frame = -1;
  ResimJitCompileStats frame_execution_jit_start{};
  ResimEventStats frame_execution_event_start{};
  u64 perf_replay_exec_count = 0;
  double perf_replay_exec_ms = 0.0;
  double perf_replay_exec_max_ms = 0.0;
  u64 perf_replay_jit_blocks = 0;
  u64 perf_replay_jit_nanoseconds = 0;
  ResimEventStats perf_replay_events{};
  u64 perf_replay_save_count = 0;
  double perf_replay_save_ms = 0.0;
  double perf_replay_save_max_ms = 0.0;
  std::optional<std::chrono::steady_clock::time_point> rollback_burst_start;
  u64 perf_rollback_burst_count = 0;
  double perf_rollback_burst_ms = 0.0;
  double perf_rollback_burst_max_ms = 0.0;
  double perf_burst_load_ms = 0.0;
  double perf_burst_load_max_ms = 0.0;
  double perf_burst_exec_ms = 0.0;
  double perf_burst_exec_max_ms = 0.0;
  double perf_burst_save_ms = 0.0;
  double perf_burst_save_max_ms = 0.0;
  double perf_burst_other_ms = 0.0;
  double perf_burst_other_max_ms = 0.0;
  double current_burst_load_ms = 0.0;
  double current_burst_exec_ms = 0.0;
  double current_burst_save_ms = 0.0;
  std::size_t current_burst_jit_blocks_invalidated = 0;
  u64 current_rollback_replays = 0;
  u64 perf_rollback_depth_total = 0;
  u64 perf_rollback_depth_max = 0;
  std::optional<std::chrono::steady_clock::time_point> last_real_advance;
  u64 perf_real_interval_count = 0;
  double perf_real_interval_ms = 0.0;
  double perf_real_interval_max_ms = 0.0;
  u64 perf_real_gaps_over_20_ms = 0;
  u64 perf_real_gaps_over_25_ms = 0;
  u64 perf_real_gaps_over_33_ms = 0;
  u64 perf_real_gaps_over_50_ms = 0;
};

GekkoManager g_manager;

void LogStartFrameStateDigest(Core::System& system, int frame)
{
  if (!g_manager.compare_confirmed_ram || !g_manager.last_advance_has_start || !g_manager.ring)
    return;

  const std::span<const u8> state = g_manager.ring->LastState();
  const State::RollbackStateLayout& layout = g_manager.ring->LastStateLayout();
  std::array<u64, State::ROLLBACK_STATE_SECTION_COUNT> sections{};
  std::size_t section_start = 0;
  for (std::size_t i = 0; i < sections.size(); ++i)
  {
    const std::size_t section_end = std::min(layout.section_ends[i], state.size());
    if (section_end < section_start)
      break;
    sections[i] = XXH3_64bits(state.data() + section_start, section_end - section_start);
    section_start = section_end;
  }

  const auto& ppc = system.GetPPCState();
  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet state trace: START save frame {} state={:016x} sections "
      "Movie={:016x},Video={:016x},CoreTiming={:016x},HW={:016x},PowerPC={:016x},"
      "Wiimote={:016x},Gecko={:016x}; ticks={}, timebase={}, pc={:08x}, npc={:08x}",
      frame, XXH3_64bits(state.data(), state.size()), sections[0], sections[1], sections[2],
      sections[3], sections[4], sections[5], sections[6], system.GetCoreTiming().GetTicks(),
      system.GetPowerPC().ReadFullTimeBaseValue(), ppc.pc, ppc.npc);
}

IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}

std::string_view DiagnosticEventName(GekkoGameEventType type)
{
  switch (type)
  {
  case GekkoSaveEvent:
    return "save";
  case GekkoLoadEvent:
    return "load";
  case GekkoAdvanceEvent:
    return "advance";
  default:
    return "other";
  }
}

void RecordDiagnosticEvent(Core::System& system, const QueuedEvent& event)
{
  if (!g_manager.compare_confirmed_ram)
    return;

  DiagnosticEvent diagnostic;
  diagnostic.type = event.type;
  diagnostic.frame = event.frame;
  diagnostic.rolling_back = event.rolling_back;
  diagnostic.running_ahead = event.running_ahead;
  diagnostic.inputs = event.inputs.empty() ? 0 : XXH3_64bits(event.inputs.data(), event.inputs.size());
  diagnostic.ticks = system.GetCoreTiming().GetTicks();
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    diagnostic.nand_mark = nand->LastJournalMark();
  g_manager.diagnostic_events.push_back(diagnostic);
  while (g_manager.diagnostic_events.size() > 16)
    g_manager.diagnostic_events.pop_front();
}

void LogDiagnosticEventHistory()
{
  std::string history;
  for (const DiagnosticEvent& event : g_manager.diagnostic_events)
  {
    if (!history.empty())
      history += ", ";
    history += fmt::format("{}(f={},rb={},ra={},in={:016x},ticks={},nand={})",
                           DiagnosticEventName(event.type), event.frame, event.rolling_back,
                           event.running_ahead, event.inputs, event.ticks, event.nand_mark);
  }
  NOTICE_LOG_FMT(CORE, "Rollback restore diagnostic: preceding events [{}]", history);
}

void WriteU32(u8* out, u32 value)
{
  for (int i = 0; i < 4; ++i)
    out[i] = static_cast<u8>(value >> (24 - i * 8));
}

void WriteU64(u8* out, u64 value)
{
  for (int i = 0; i < 8; ++i)
    out[i] = static_cast<u8>(value >> (56 - i * 8));
}

u32 ReadU32(const u8* in)
{
  u32 value = 0;
  for (int i = 0; i < 4; ++i)
    value = (value << 8) | in[i];
  return value;
}

u64 ReadU64(const u8* in)
{
  u64 value = 0;
  for (int i = 0; i < 8; ++i)
    value = (value << 8) | in[i];
  return value;
}

std::array<u8, RAM_DIAGNOSTIC_PACKET_SIZE> MakeRamDiagnosticPacket(u8 type,
                                                                   const RamDigest& digest)
{
  std::array<u8, RAM_DIAGNOSTIC_PACKET_SIZE> packet{};
  std::copy(RAM_DIAGNOSTIC_MAGIC.begin(), RAM_DIAGNOSTIC_MAGIC.end(), packet.begin());
  packet[4] = RAM_DIAGNOSTIC_VERSION;
  packet[5] = type;
  packet[6] = static_cast<u8>(g_manager.local_player);
  WriteU32(packet.data() + 8, static_cast<u32>(digest.frame));
  WriteU64(packet.data() + 12, digest.root);
  WriteU64(packet.data() + 20, digest.mem1);
  WriteU64(packet.data() + 28, digest.mem2);
  WriteU64(packet.data() + 36, digest.aram);
  WriteU64(packet.data() + 44, digest.fake_vmem);
  WriteU64(packet.data() + 52, digest.l1);
  WriteU64(packet.data() + 60, digest.inputs);
  WriteU64(packet.data() + 68, digest.state);
  WriteU64(packet.data() + 76, digest.core_ticks);
  WriteU64(packet.data() + 84, digest.timebase);
  WriteU64(packet.data() + 92, digest.fake_tb_start_value);
  WriteU64(packet.data() + 100, digest.fake_tb_start_ticks);
  WriteU32(packet.data() + 108, digest.decrementer);
  for (std::size_t i = 0; i < digest.state_sections.size(); ++i)
    WriteU64(packet.data() + 112 + i * sizeof(u64), digest.state_sections[i]);
  return packet;
}

void SendRamDiagnosticPacket(const ENetAddress& endpoint, u8 type, const RamDigest& digest)
{
  const auto packet = MakeRamDiagnosticPacket(type, digest);
  Common::ENet::SendRollbackDatagram(endpoint, packet.data(), packet.size());
}

void CompareRamDigest(int remote_player, const ENetAddress* sender, const RamDigest& remote)
{
  const auto local_it = g_manager.ram_digests.find(remote.frame);
  if (local_it == g_manager.ram_digests.end())
    return;

  const RamDigest& local = local_it->second;
  const bool ram_matches = local.root == remote.root;
  const bool timing_matches =
      local.core_ticks == remote.core_ticks && local.timebase == remote.timebase &&
      local.fake_tb_start_value == remote.fake_tb_start_value &&
      local.fake_tb_start_ticks == remote.fake_tb_start_ticks &&
      local.decrementer == remote.decrementer;
  // PointerWrap serializes some trivially-copyable structs with their alignment padding. For
  // example, IOS FSCore::Handle has two padding bytes between gid and uid whose values can differ
  // even though every meaningful field matches. Keep the state and section hashes in the report,
  // but do not stop on them alone; RAM and the explicit timing fields are padding-free.
  if (ram_matches && timing_matches)
    return;

  std::string differing_sections;
  for (std::size_t i = 0; i < local.state_sections.size(); ++i)
  {
    if (local.state_sections[i] == remote.state_sections[i])
      continue;
    if (!differing_sections.empty())
      differing_sections += ", ";
    differing_sections += fmt::format("{}={:016x}/{:016x}", ROLLBACK_STATE_SECTION_NAMES[i],
                                      local.state_sections[i], remote.state_sections[i]);
  }
  if (differing_sections.empty())
    differing_sections = "none";

  if (!g_manager.first_ram_mismatch_frame || remote.frame < *g_manager.first_ram_mismatch_frame)
    g_manager.first_ram_mismatch_frame = remote.frame;

  if (!g_manager.ram_mismatch_details_frame ||
      remote.frame < *g_manager.ram_mismatch_details_frame)
  {
    g_manager.ram_mismatch_details_frame = remote.frame;
    NOTICE_LOG_FMT(
        CORE,
        "Rollback RAM diagnostic: first confirmed mismatch at frame {} versus player {}: root "
        "local={:016x} remote={:016x}; MEM1 {:016x}/{:016x}; MEM2 {:016x}/{:016x}; "
        "ARAM {:016x}/{:016x}; FakeVMEM {:016x}/{:016x}; L1 {:016x}/{:016x}; "
        "state {:016x}/{:016x}; inputs {} ({:016x}/{:016x}); "
        "timing ticks {}/{} TB {:#x}/{:#x} fake-TB-value {:#x}/{:#x} fake-TB-ticks {}/{} "
        "DEC {:#x}/{:#x}; differing state sections [{}]",
        remote.frame, remote_player, local.root, remote.root, local.mem1, remote.mem1, local.mem2,
        remote.mem2, local.aram, remote.aram, local.fake_vmem, remote.fake_vmem, local.l1,
        remote.l1, local.state, remote.state,
        local.inputs == remote.inputs ? "match" : "DIFFER", local.inputs, remote.inputs,
        local.core_ticks, remote.core_ticks, local.timebase, remote.timebase,
        local.fake_tb_start_value, remote.fake_tb_start_value, local.fake_tb_start_ticks,
        remote.fake_tb_start_ticks, local.decrementer, remote.decrementer, differing_sections);
    LogDiagnosticEventHistory();
  }

  // Tell the other peer explicitly so it pauses and preserves the same frame even if its copy of
  // the corresponding digest datagram was lost.
  if (sender)
  {
    for (int retry = 0; retry < 3; ++retry)
      SendRamDiagnosticPacket(*sender, RAM_DIAGNOSTIC_MISMATCH, local);
  }
  else
  {
    for (const ENetAddress& endpoint : g_manager.remote_endpoints)
    {
      for (int retry = 0; retry < 3; ++retry)
        SendRamDiagnosticPacket(endpoint, RAM_DIAGNOSTIC_MISMATCH, local);
    }
  }
}

bool HandleRamDiagnosticDatagram(const Common::ENet::RollbackDatagram& datagram)
{
  const auto& payload = datagram.payload;
  if (payload.size() < RAM_DIAGNOSTIC_MAGIC.size() ||
      !std::equal(RAM_DIAGNOSTIC_MAGIC.begin(), RAM_DIAGNOSTIC_MAGIC.end(), payload.begin()))
  {
    return false;
  }

  // Always consume our private packet type so GekkoNet never tries to parse it as protocol data.
  if (payload.size() < 12 || payload[4] != RAM_DIAGNOSTIC_VERSION)
    return true;
  if (!g_manager.compare_confirmed_ram)
  {
    g_manager.compare_confirmed_ram = true;
    NOTICE_LOG_FMT(CORE,
                   "Rollback RAM diagnostic: enabled by peer; confirmed snapshots will now be "
                   "compared");
  }

  RamDigest remote{};
  const u8 type = payload[5];
  const int remote_player = payload[6];
  remote.frame = static_cast<int>(ReadU32(payload.data() + 8));
  if (type == RAM_DIAGNOSTIC_MISMATCH)
  {
    if (!g_manager.first_ram_mismatch_frame || remote.frame < *g_manager.first_ram_mismatch_frame)
      g_manager.first_ram_mismatch_frame = remote.frame;
    NOTICE_LOG_FMT(CORE, "Rollback RAM diagnostic: player {} reported mismatch frame {}; pausing "
                         "to restore and dump it",
                   remote_player, remote.frame);
    return true;
  }
  if (type != RAM_DIAGNOSTIC_DIGEST || payload.size() != RAM_DIAGNOSTIC_PACKET_SIZE ||
      remote_player == g_manager.local_player)
  {
    return true;
  }

  remote.root = ReadU64(payload.data() + 12);
  remote.mem1 = ReadU64(payload.data() + 20);
  remote.mem2 = ReadU64(payload.data() + 28);
  remote.aram = ReadU64(payload.data() + 36);
  remote.fake_vmem = ReadU64(payload.data() + 44);
  remote.l1 = ReadU64(payload.data() + 52);
  remote.inputs = ReadU64(payload.data() + 60);
  remote.state = ReadU64(payload.data() + 68);
  remote.core_ticks = ReadU64(payload.data() + 76);
  remote.timebase = ReadU64(payload.data() + 84);
  remote.fake_tb_start_value = ReadU64(payload.data() + 92);
  remote.fake_tb_start_ticks = ReadU64(payload.data() + 100);
  remote.decrementer = ReadU32(payload.data() + 108);
  for (std::size_t i = 0; i < remote.state_sections.size(); ++i)
    remote.state_sections[i] = ReadU64(payload.data() + 112 + i * sizeof(u64));
  g_manager.remote_ram_digests[{remote_player, remote.frame}] = remote;
  return true;
}

void FillRamRegionHashes(Core::System& system, RamDigest* digest)
{
  auto& memory = system.GetMemory();
  digest->mem1 = XXH3_64bits(memory.GetRAM(), memory.GetRamSize());
  digest->mem2 = memory.GetEXRAM() ? XXH3_64bits(memory.GetEXRAM(), memory.GetExRamSize()) : 0;
  if (!system.IsWii())
  {
    auto& dsp = system.GetDSP();
    digest->aram = XXH3_64bits(dsp.GetARAMPtr(), dsp.GetARAMSize());
  }
  digest->fake_vmem = memory.GetFakeVMEM() ?
                          XXH3_64bits(memory.GetFakeVMEM(), memory.GetFakeVMemSize()) :
                          0;
  digest->l1 = XXH3_64bits(memory.GetL1Cache(), memory.GetL1CacheSize());
  const std::array<u64, 5> regions{digest->mem1, digest->mem2, digest->aram, digest->fake_vmem,
                                   digest->l1};
  digest->root = XXH3_64bits(regions.data(), sizeof(regions));
}

void VerifyRestoredRam(Core::System& system, int frame)
{
  if (!g_manager.compare_confirmed_ram)
    return;

  const auto expected_it = g_manager.ram_digests.find(frame);
  if (expected_it == g_manager.ram_digests.end())
    return;

  ++g_manager.perf_restore_checks;
  RamDigest actual{};
  actual.frame = frame;
  FillRamRegionHashes(system, &actual);
  const RamDigest& expected = expected_it->second;
  if (actual.root == expected.root)
    return;

  ++g_manager.perf_restore_mismatches;
  if (g_manager.restore_mismatch_reported)
    return;
  g_manager.restore_mismatch_reported = true;

  const s64 snapshot_key = static_cast<s64>(frame) + 1;
  const std::optional<u64> expected_nand = g_manager.ring->NandJournalMarkForFrame(snapshot_key);
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  const std::optional<u64> actual_nand = nand ? std::optional<u64>(nand->LastJournalMark()) :
                                               std::nullopt;
  NOTICE_LOG_FMT(
      CORE,
      "Rollback restore diagnostic: RESTORE MISMATCH frame {} key {} root {:016x}/{:016x}; "
      "MEM1 {:016x}/{:016x}; MEM2 {:016x}/{:016x}; ARAM {:016x}/{:016x}; "
      "FakeVMEM {:016x}/{:016x}; L1 {:016x}/{:016x}; NAND mark {}/{}",
      frame, snapshot_key, expected.root, actual.root, expected.mem1, actual.mem1, expected.mem2,
      actual.mem2, expected.aram, actual.aram, expected.fake_vmem, actual.fake_vmem, expected.l1,
      actual.l1, expected_nand ? fmt::format("{}", *expected_nand) : "unavailable",
      actual_nand ? fmt::format("{}", *actual_nand) : "unavailable");
  LogDiagnosticEventHistory();
}

void RecordRamDigest(Core::System& system, int frame)
{
  if (!g_manager.compare_confirmed_ram)
    return;

  RamDigest digest{};
  digest.frame = frame;
  FillRamRegionHashes(system, &digest);
  digest.inputs = g_manager.last_advance_input_hash;
  const std::span<const u8> state = g_manager.ring->LastState();
  digest.state = XXH3_64bits(state.data(), state.size());
  const State::RollbackStateLayout& layout = g_manager.ring->LastStateLayout();
  std::size_t section_start = 0;
  for (std::size_t i = 0; i < digest.state_sections.size(); ++i)
  {
    const std::size_t section_end = std::min(layout.section_ends[i], state.size());
    if (section_end < section_start)
      break;
    digest.state_sections[i] =
        XXH3_64bits(state.data() + section_start, section_end - section_start);
    section_start = section_end;
  }
  auto& core_timing = system.GetCoreTiming();
  digest.core_ticks = core_timing.GetTicks();
  digest.timebase = system.GetPowerPC().ReadFullTimeBaseValue();
  digest.fake_tb_start_value = core_timing.GetFakeTBStartValue();
  digest.fake_tb_start_ticks = core_timing.GetFakeTBStartTicks();
  digest.decrementer = system.GetSystemTimers().GetFakeDecrementer();
  if (frame == -1)
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback startup diagnostic: frame -1 root={:016x} state={:016x} ticks={} "
                   "TB={:#x} fake-TB-value={:#x} fake-TB-ticks={} DEC={:#x}",
                   digest.root, digest.state, digest.core_ticks, digest.timebase,
                   digest.fake_tb_start_value, digest.fake_tb_start_ticks, digest.decrementer);
  }
  g_manager.ram_digests[frame] = digest;
  while (g_manager.ram_digests.size() > 128)
    g_manager.ram_digests.erase(g_manager.ram_digests.begin());
  while (g_manager.remote_ram_digests.size() > 512)
    g_manager.remote_ram_digests.erase(g_manager.remote_ram_digests.begin());
}

void LogSparseStateCheckpoint(Core::System& system, int frame)
{
  if (!g_manager.compare_confirmed_ram || frame < 0 || frame % 60 != 0 || !g_manager.ring)
    return;

  RamDigest digest{};
  FillRamRegionHashes(system, &digest);
  const std::span<const u8> state = g_manager.ring->LastState();
  const State::RollbackStateLayout& layout = g_manager.ring->LastStateLayout();
  std::size_t section_start = 0;
  for (std::size_t i = 0; i < digest.state_sections.size(); ++i)
  {
    const std::size_t section_end = std::min(layout.section_ends[i], state.size());
    if (section_end < section_start)
      break;
    digest.state_sections[i] =
        XXH3_64bits(state.data() + section_start, section_end - section_start);
    section_start = section_end;
  }

  NOTICE_LOG_FMT(
      CORE,
      "Rollback sparse checkpoint: frame {} resim={} input={:016x} MEM1={:016x} MEM2={:016x} "
      "L1={:016x} state={:016x} sections Movie={:016x},Video={:016x},CoreTiming={:016x},"
      "HW={:016x},PowerPC={:016x},Wiimote={:016x},Gecko={:016x}",
      frame, Rollback::IsResimulating(), g_manager.last_advance_input_hash, digest.mem1,
      digest.mem2, digest.l1, XXH3_64bits(state.data(), state.size()), digest.state_sections[0],
      digest.state_sections[1], digest.state_sections[2], digest.state_sections[3],
      digest.state_sections[4], digest.state_sections[5], digest.state_sections[6]);
}

void CompareSettledRamDigests()
{
  if (!g_manager.compare_confirmed_ram)
    return;
  for (const auto& [key, remote] : g_manager.remote_ram_digests)
    CompareRamDigest(key.first, nullptr, remote);
}

void SendConfirmedRamDigests()
{
  if (!g_manager.compare_confirmed_ram || !g_manager.session ||
      g_manager.remote_endpoints.empty())
  {
    return;
  }

  int confirmed = std::numeric_limits<int>::max();
  for (const int handle : g_manager.player_handles)
    confirmed = std::min(confirmed, gekko_last_received_frame(g_manager.session, handle));
  if (confirmed < -1)
    return;

  for (auto& [frame, digest] : g_manager.ram_digests)
  {
    if (frame > confirmed || digest.sends >= 3)
      continue;
    for (const ENetAddress& endpoint : g_manager.remote_endpoints)
      SendRamDiagnosticPacket(endpoint, RAM_DIAGNOSTIC_DIGEST, digest);
    ++digest.sends;
  }
}

bool WriteRamRegion(const std::string& path, const u8* data, std::size_t size)
{
  return File::IOFile(path, "wb").WriteBytes(data, size);
}

bool DumpFirstRamMismatch(Core::System& system)
{
  if (!g_manager.compare_confirmed_ram || g_manager.ram_diagnostic_dumped ||
      !g_manager.first_ram_mismatch_frame || !g_manager.ring)
  {
    return false;
  }

  const int frame = *g_manager.first_ram_mismatch_frame;
  if (!g_manager.ring->Load(system, static_cast<s64>(frame) + 1))
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback RAM diagnostic: mismatch frame {} fell out of snapshot ring; "
                   "pausing without a dump",
                   frame);
    g_manager.ram_diagnostic_dumped = true;
    Core::SetState(system, Core::State::Paused);
    return true;
  }

  auto& memory = system.GetMemory();
  const std::string base =
      fmt::format("{}rollback-ram-session-{}-frame-{}-p{}", File::GetUserPath(D_LOGS_IDX),
                  g_manager.session_id, frame, g_manager.local_player);
  const bool mem1_ok = WriteRamRegion(base + "-mem1.bin", memory.GetRAM(), memory.GetRamSize());
  const bool mem2_ok = !memory.GetEXRAM() ||
                       WriteRamRegion(base + "-mem2.bin", memory.GetEXRAM(), memory.GetExRamSize());
  bool aram_ok = true;
  if (!system.IsWii())
  {
    auto& dsp = system.GetDSP();
    aram_ok = WriteRamRegion(base + "-aram.bin", dsp.GetARAMPtr(), dsp.GetARAMSize());
  }
  const bool fake_vmem_ok =
      !memory.GetFakeVMEM() || WriteRamRegion(base + "-fakevmem.bin", memory.GetFakeVMEM(),
                                              memory.GetFakeVMemSize());
  const bool l1_ok =
      WriteRamRegion(base + "-l1.bin", memory.GetL1Cache(), memory.GetL1CacheSize());
  const std::span<const u8> state =
      g_manager.ring->StateForFrame(static_cast<s64>(frame) + 1);
  const bool state_ok =
      !state.empty() && WriteRamRegion(base + "-state.bin", state.data(), state.size());

  g_manager.ram_diagnostic_dumped = true;
  NOTICE_LOG_FMT(CORE,
                 "Rollback RAM diagnostic: restored confirmed frame {} and dumped P{} RAM to "
                 "{}-[mem1|mem2|aram|fakevmem|l1|state].bin "
                 "(write_ok={}/{}/{}/{}/{}/{}); emulation paused",
                 frame, g_manager.local_player, base, mem1_ok, mem2_ok, aram_ok, fake_vmem_ok,
                 l1_ok, state_ok);
  Core::SetState(system, Core::State::Paused);
  return true;
}

}  // namespace

WirePad EncodePad(const GCPadStatus& status)
{
  WirePad pad{};
  const u16 buttons = status.button & PAD_WIRE_BUTTONS;
  pad.data[0] = static_cast<u8>(buttons >> 8);
  pad.data[1] = static_cast<u8>(buttons & 0xFF);
  pad.data[2] = static_cast<u8>(status.stickX - GCPadStatus::MAIN_STICK_CENTER_X);
  pad.data[3] = static_cast<u8>(status.stickY - GCPadStatus::MAIN_STICK_CENTER_Y);
  pad.data[4] = static_cast<u8>(status.substickX - GCPadStatus::C_STICK_CENTER_X);
  pad.data[5] = static_cast<u8>(status.substickY - GCPadStatus::C_STICK_CENTER_Y);
  pad.data[6] = status.triggerLeft;
  pad.data[7] = status.triggerRight;
  return pad;
}

bool WirePadHasStart(const WirePad& pad)
{
  const u16 buttons = static_cast<u16>((pad.data[0] << 8) | pad.data[1]);
  return (buttons & PAD_BUTTON_START) != 0;
}

GCPadStatus DecodePad(const WirePad& pad)
{
  GCPadStatus status{};
  status.button = static_cast<u16>((pad.data[0] << 8) | pad.data[1]) & PAD_WIRE_BUTTONS;
  status.stickX = static_cast<u8>(pad.data[2] + GCPadStatus::MAIN_STICK_CENTER_X);
  status.stickY = static_cast<u8>(pad.data[3] + GCPadStatus::MAIN_STICK_CENTER_Y);
  status.substickX = static_cast<u8>(pad.data[4] + GCPadStatus::C_STICK_CENTER_X);
  status.substickY = static_cast<u8>(pad.data[5] + GCPadStatus::C_STICK_CENTER_Y);
  status.triggerLeft = pad.data[6];
  status.triggerRight = pad.data[7];
  status.analogA = 0;
  status.analogB = 0;
  status.isConnected = true;
  return status;
}

std::optional<GCPadStatus> GetRollbackPad(int port)
{
  if (!g_manager.active.load(std::memory_order_relaxed))
    return std::nullopt;

  if (port >= 0 && port < MAX_PORTS)
  {
    std::lock_guard lk(g_manager.mutex);
    return g_manager.latched_pads[port];
  }
  return std::nullopt;
}

bool IsGekkoSessionActive()
{
  return g_manager.active.load(std::memory_order_relaxed);
}

void SetGekkoLocalDelay(int local_delay)
{
  const int clamped_delay = std::clamp(local_delay, 0, 60);
  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.session || g_manager.local_handle < 0)
    return;

  const int previous_delay = g_manager.configured_local_delay;
  if (previous_delay == clamped_delay)
    return;

  gekko_set_local_delay(g_manager.session, g_manager.local_handle,
                        static_cast<unsigned char>(clamped_delay));
  g_manager.configured_local_delay = clamped_delay;
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  NOTICE_LOG_FMT(CORE, "GekkoNet: Live local input delay changed {} -> {} at sample frame {}",
                 previous_delay, clamped_delay, g_manager.local_input_frame);
}

bool StartGekkoSession(const std::string& game_name, u32 session_id, int players, int local_player,
                       const std::vector<std::string>& player_endpoints, int local_delay,
                       int prediction_window, bool debug_p2_cstick, bool simulate_remote_p2,
                       bool stress_test, bool compare_confirmed_ram)
{
  StopGekkoSession();

  std::lock_guard lk(g_manager.mutex);

  if (players < 1 || players > 4 || local_player < 1 || local_player > players)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Invalid session parameters (players={}, local={})", players,
                  local_player);
    return false;
  }
  if (simulate_remote_p2 && (players != 2 || local_player != 1))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: simulated remote player 2 requires a two-player host session");
    return false;
  }
  if (stress_test && !simulate_remote_p2)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: rollback stress test requires the simulated remote peer");
    return false;
  }

  if (!gekko_create(&g_manager.session, GekkoGameSession))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create session");
    return false;
  }

  const int clamped_delay = std::clamp(local_delay, 0, 60);
  const int clamped_rollback_window = std::clamp(prediction_window, 1, 10);
  // Gekko allows prediction only while prediction_window > frames_already_predicted. The stress
  // transport therefore gets one internal prediction slot beyond the selected rollback distance,
  // or both simulated peers can reach the cap and stop the emulated clock that releases packets.
  const int clamped_prediction =
      stress_test ? clamped_rollback_window + 1 : clamped_rollback_window;

  GekkoConfig config{};
  config.num_players = static_cast<unsigned char>(players);
  config.max_spectators = 0;
  config.input_prediction_window = static_cast<unsigned char>(clamped_prediction);
  config.spectator_delay = 0;
  config.input_size = static_cast<unsigned int>(sizeof(WirePad));
  config.state_size = 0;  // State is maintained internally in Dolphin's SnapshotRing
  // GekkoNet's limited-saving mode rebuilds confirmed snapshots by loading and replaying frames.
  // That is counterproductive for whole-emulator states, whose loads and replay frames are much
  // more expensive than a small game's state copy. Keep per-frame snapshots until GekkoNet has an
  // Orca-style sparse forward-snapshot policy.
  config.limited_saving = false;
  // Dolphin's rollback snapshots deliberately do not provide hashes to GekkoNet, so its checksum
  // comparison cannot produce meaningful desync results.
  config.desync_detection = false;
  config.check_distance = 10;

  gekko_start(g_manager.session, &config);

  GekkoNetAdapter* adapter = nullptr;
  if (simulate_remote_p2)
    adapter = &s_main_simulated_adapter;
  else
  {
    if (!Common::ENet::StartRollbackDatagrams(session_id))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: no shared NetPlay UDP socket is registered");
      gekko_destroy(&g_manager.session);
      return false;
    }
    adapter = &s_shared_adapter;
  }

  if (!adapter)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create network adapter");
    gekko_destroy(&g_manager.session);
    return false;
  }
  g_manager.native_adapter_active = !simulate_remote_p2;
  gekko_net_adapter_set(g_manager.session, adapter);
  const auto destroy_failed_session = [&] {
    gekko_destroy(&g_manager.session);
    if (g_manager.native_adapter_active)
    {
      Common::ENet::StopRollbackDatagrams();
      g_manager.native_adapter_active = false;
    }
  };

  gekko_set_runahead(g_manager.session, 0);

  g_manager.players = players;
  g_manager.local_player = local_player;
  // Capture the first local device before NetPlay's boot layer remaps local SI devices to their
  // in-game seats. This remains port 0 on every client regardless of its network player number.
  g_manager.local_uses_gc_adapter =
      Config::Get(Config::GetInfoForSIDevice(0)) == SerialInterface::SIDEVICE_WIIU_ADAPTER;
  g_manager.debug_p2_cstick = debug_p2_cstick;
  g_manager.simulate_remote_p2 = simulate_remote_p2;
  g_manager.stress_test = stress_test;
  g_manager.compare_confirmed_ram = compare_confirmed_ram && !simulate_remote_p2 && players > 1;
  g_manager.ram_diagnostic_dumped = false;
  g_manager.frame_boundary = FrameBoundary::VINewField;
  g_manager.vi_boundary_pending.store(false, std::memory_order_relaxed);
  g_manager.configured_local_delay = clamped_delay;
  g_manager.session_id = session_id;
  g_manager.local_input_frame = 0;
  // GetRollbackPad is active throughout boot, before Gekko emits the first Advance event. Never
  // expose pads left latched at the end of the previous session to the next game's SI startup.
  g_manager.latched_pads.fill(GCPadStatus{});
  g_manager.local_handle = -1;
  g_manager.simulated_peer_p2_handle = -1;
  g_manager.remote_handle = -1;
  g_manager.player_handles.assign(players, -1);
  g_manager.remote_endpoints.clear();
  g_manager.pending_events.clear();
  g_manager.sampled_local_inputs.clear();
  g_manager.ram_digests.clear();
  g_manager.remote_ram_digests.clear();
  g_manager.first_ram_mismatch_frame.reset();
  g_manager.ram_mismatch_details_frame.reset();
  g_manager.last_advance_input_hash = 0;
  g_manager.last_advance_has_start = false;
  g_manager.last_sampled_start = false;
  g_manager.diagnostic_events.clear();
  g_manager.restore_mismatch_reported = false;
  g_manager.perf_restore_checks = 0;
  g_manager.perf_restore_mismatches = 0;
  g_manager.perf_window_start = {};
  g_manager.perf_dirty_pages_start = DirtyPages::GetCounters();
  g_manager.perf_real_frames = 0;
  g_manager.perf_replay_frames = 0;
  g_manager.perf_save_count = 0;
  g_manager.perf_load_count = 0;
  g_manager.perf_pump_count = 0;
  g_manager.perf_save_ms = 0.0;
  g_manager.perf_save_max_ms = 0.0;
  g_manager.perf_load_ms = 0.0;
  g_manager.perf_load_max_ms = 0.0;
  g_manager.perf_pump_ms = 0.0;
  g_manager.perf_pump_max_ms = 0.0;
  g_manager.perf_save_phases = {};
  g_manager.perf_load_phases = {};
  g_manager.last_local_wire.reset();
  g_manager.perf_local_input_changes = 0;
  g_manager.perf_local_byte_changes.fill(0);
  g_manager.last_applied_local_wire.reset();
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  g_manager.frame_execution_start.reset();
  g_manager.frame_execution_resim = false;
  g_manager.frame_execution_gekko_frame = -1;
  g_manager.frame_execution_jit_start = GetResimJitCompileStats();
  g_manager.frame_execution_event_start = GetResimEventStats();
  g_manager.perf_replay_exec_count = 0;
  g_manager.perf_replay_exec_ms = 0.0;
  g_manager.perf_replay_exec_max_ms = 0.0;
  g_manager.perf_replay_jit_blocks = 0;
  g_manager.perf_replay_jit_nanoseconds = 0;
  g_manager.perf_replay_events = {};
  g_manager.perf_replay_save_count = 0;
  g_manager.perf_replay_save_ms = 0.0;
  g_manager.perf_replay_save_max_ms = 0.0;
  g_manager.rollback_burst_start.reset();
  g_manager.perf_rollback_burst_count = 0;
  g_manager.perf_rollback_burst_ms = 0.0;
  g_manager.perf_rollback_burst_max_ms = 0.0;
  g_manager.perf_burst_load_ms = 0.0;
  g_manager.perf_burst_load_max_ms = 0.0;
  g_manager.perf_burst_exec_ms = 0.0;
  g_manager.perf_burst_exec_max_ms = 0.0;
  g_manager.perf_burst_save_ms = 0.0;
  g_manager.perf_burst_save_max_ms = 0.0;
  g_manager.perf_burst_other_ms = 0.0;
  g_manager.perf_burst_other_max_ms = 0.0;
  g_manager.current_burst_load_ms = 0.0;
  g_manager.current_burst_exec_ms = 0.0;
  g_manager.current_burst_save_ms = 0.0;
  g_manager.current_burst_jit_blocks_invalidated = 0;
  g_manager.current_rollback_replays = 0;
  g_manager.perf_rollback_depth_total = 0;
  g_manager.perf_rollback_depth_max = 0;
  g_manager.last_real_advance.reset();
  g_manager.perf_real_interval_count = 0;
  g_manager.perf_real_interval_ms = 0.0;
  g_manager.perf_real_interval_max_ms = 0.0;
  g_manager.perf_real_gaps_over_20_ms = 0;
  g_manager.perf_real_gaps_over_25_ms = 0;
  g_manager.perf_real_gaps_over_33_ms = 0;
  g_manager.perf_real_gaps_over_50_ms = 0;
  g_manager.throttle_reference_before_load.reset();

  for (int p = 1; p <= players; ++p)
  {
    if (p == local_player)
    {
      const int handle = gekko_add_actor(g_manager.session, GekkoLocalPlayer, nullptr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add local actor");
        destroy_failed_session();
        return false;
      }
      g_manager.local_handle = handle;
      g_manager.player_handles[p - 1] = handle;
      gekko_set_local_delay(g_manager.session, handle, static_cast<unsigned char>(clamped_delay));
    }
    else if (simulate_remote_p2 && p == 2)
    {
      GekkoNetAddress addr{const_cast<char*>(SIMULATED_P2_ADDRESS.data()),
                           static_cast<unsigned int>(SIMULATED_P2_ADDRESS.size())};
      const int handle = gekko_add_actor(g_manager.session, GekkoRemotePlayer, &addr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add simulated remote player 2 actor");
        destroy_failed_session();
        return false;
      }
      g_manager.player_handles[p - 1] = handle;
      g_manager.remote_handle = handle;
    }
    else
    {
      if (p >= static_cast<int>(player_endpoints.size()) || player_endpoints[p].empty())
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Missing native endpoint for player {}", p);
        destroy_failed_session();
        return false;
      }
      NOTICE_LOG_FMT(CORE, "GekkoNet: Native player {} endpoint {}", p, player_endpoints[p]);
      ENetAddress endpoint{};
      if (!ParsePeerEndpoint(player_endpoints[p], &endpoint))
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Invalid native endpoint for player {}: {}", p,
                      player_endpoints[p]);
        destroy_failed_session();
        return false;
      }
      GekkoNetAddress addr{&endpoint, sizeof(endpoint)};
      const int handle = gekko_add_actor(g_manager.session, GekkoRemotePlayer, &addr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add remote actor");
        destroy_failed_session();
        return false;
      }
      if (g_manager.remote_handle < 0)
        g_manager.remote_handle = handle;
      g_manager.player_handles[p - 1] = handle;
      g_manager.remote_endpoints.push_back(endpoint);
    }
  }

  if (simulate_remote_p2)
  {
    ResetSimulatedLink();
    s_stress_mode = stress_test;
    s_stress_packet_delay_frames =
        stress_test ? static_cast<u64>(clamped_rollback_window) : 0;
    if (!gekko_create(&g_manager.simulated_peer_session, GekkoGameSession))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create simulated player 2 peer session");
      destroy_failed_session();
      return false;
    }
    gekko_start(g_manager.simulated_peer_session, &config);
    gekko_net_adapter_set(g_manager.simulated_peer_session, &s_fake_simulated_adapter);
    gekko_set_runahead(g_manager.simulated_peer_session, 0);

    GekkoNetAddress p1_addr{const_cast<char*>(SIMULATED_P1_ADDRESS.data()),
                            static_cast<unsigned int>(SIMULATED_P1_ADDRESS.size())};
    const int fake_remote_p1 =
        gekko_add_actor(g_manager.simulated_peer_session, GekkoRemotePlayer, &p1_addr);
    const int fake_local_p2 =
        gekko_add_actor(g_manager.simulated_peer_session, GekkoLocalPlayer, nullptr);
    if (fake_remote_p1 < 0 || fake_local_p2 < 0)
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add actors to simulated player 2 peer");
      gekko_destroy(&g_manager.simulated_peer_session);
      destroy_failed_session();
      return false;
    }
    g_manager.simulated_peer_p2_handle = fake_local_p2;
    gekko_set_local_delay(g_manager.simulated_peer_session, fake_local_p2,
                          static_cast<unsigned char>(stress_test ? 0 : clamped_delay));
  }

  g_manager.tracked_bitmap_clear = Config::Get(Config::NETPLAY_ROLLBACK_TRACKED_BITMAP_CLEAR);
  g_manager.full_scan_benchmark = Config::Get(Config::NETPLAY_ROLLBACK_FULL_SCAN_BENCHMARK);
  g_manager.ring = std::make_unique<SnapshotRing>(RING_SNAPSHOT_SLOTS,
                                                  g_manager.tracked_bitmap_clear,
                                                  g_manager.full_scan_benchmark);
  g_manager.stop_requested.store(false, std::memory_order_relaxed);
  g_manager.vi_boundary_pending.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_tail.store(false, std::memory_order_relaxed);
  g_manager.active.store(true, std::memory_order_release);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet: Started {} shared NetPlay UDP session (players={}, local={}, session={}, "
      "delay={}, "
      "rollback_window={}, gekko_prediction_window={}, local_input_source={}, "
      "debug_p2_cstick={}, simulate_remote_p2={}, simulated_one_way_latency_ms={}, "
      "stress_every_3f={}, stress_rollback_frames={}, tracked_bitmap_clear={}, "
      "full_scan_benchmark={}, compare_confirmed_ram={}, frame_boundary={})",
      game_name, players, local_player, session_id, clamped_delay, clamped_rollback_window,
      clamped_prediction,
      g_manager.local_uses_gc_adapter ? "gc_adapter" : "emulated_pad", debug_p2_cstick,
      simulate_remote_p2, simulate_remote_p2 && !stress_test ? SIMULATED_P2_LATENCY.count() : 0,
      stress_test, s_stress_packet_delay_frames, g_manager.tracked_bitmap_clear,
      g_manager.full_scan_benchmark,
      g_manager.compare_confirmed_ram, FrameBoundaryName(g_manager.frame_boundary));
  return true;
}

void StopGekkoSession()
{
  std::lock_guard lk(g_manager.mutex);
  const bool had_session = g_manager.active.load(std::memory_order_relaxed) || g_manager.session ||
                           g_manager.simulated_peer_session || g_manager.native_adapter_active;

  g_manager.stop_requested.store(true, std::memory_order_relaxed);
  g_manager.active.store(false, std::memory_order_release);
  g_manager.vi_boundary_pending.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_tail.store(false, std::memory_order_relaxed);

  if (g_manager.session)
  {
    gekko_destroy(&g_manager.session);
    g_manager.session = nullptr;
  }
  if (g_manager.simulated_peer_session)
  {
    gekko_destroy(&g_manager.simulated_peer_session);
    g_manager.simulated_peer_session = nullptr;
  }
  if (g_manager.native_adapter_active)
  {
    Common::ENet::StopRollbackDatagrams();
    g_manager.native_adapter_active = false;
  }
  ResetSimulatedLink();
  g_manager.pending_events.clear();
  g_manager.throttle_reference_before_load.reset();
  if (g_manager.ring)
    g_manager.ring->Reset(Core::System::GetInstance());
  g_manager.ring.reset();
  Rollback::SetResimulating(false);
  if (had_session)
    NOTICE_LOG_FMT(CORE, "GekkoNet: Session stopped");
}

static void ApplyDebugCStickPattern(GCPadStatus* status, u64 frame)
{
  // Change to the next of eight directions every two frames. Diagonals are normalized to
  // approximately the same radius as cardinal directions.
  constexpr int DIRECTION_FRAMES = 2;
  constexpr std::array<std::pair<int, int>, 8> DIRECTIONS{{
      {90, 0},
      {64, -64},
      {0, -90},
      {-64, -64},
      {-90, 0},
      {-64, 64},
      {0, 90},
      {64, 64},
  }};

  const auto [x, y] = DIRECTIONS[(frame / DIRECTION_FRAMES) % DIRECTIONS.size()];
  status->substickX = static_cast<u8>(GCPadStatus::C_STICK_CENTER_X + x);
  status->substickY = static_cast<u8>(GCPadStatus::C_STICK_CENTER_Y + y);

  // Mash both triggers on even frames and release them on odd frames. Override both the digital
  // click and analog pressure so the wire input changes unambiguously every frame.
  constexpr u16 TRIGGER_BUTTONS = PAD_TRIGGER_L | PAD_TRIGGER_R;
  const bool triggers_pressed = (frame & 1) == 0;
  if (triggers_pressed)
    status->button |= TRIGGER_BUTTONS;
  else
    status->button &= ~TRIGGER_BUTTONS;
  status->triggerLeft = triggers_pressed ? 255 : 0;
  status->triggerRight = triggers_pressed ? 255 : 0;
}

static void ApplyStressCStickPattern(GCPadStatus* status, u64 frame)
{
  // A transition every three frames defeats repeat-last-input prediction. Alternating left/right
  // avoids a neutral interval whose first prediction could accidentally be correct.
  const bool right = ((frame / STRESS_INPUT_PERIOD_FRAMES) & 1) == 0;
  status->substickX = static_cast<u8>(GCPadStatus::C_STICK_CENTER_X + (right ? 90 : -90));
  status->substickY = GCPadStatus::C_STICK_CENTER_Y;
}

static void SubmitLocalInput()
{
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  g_controller_interface.UpdateInput();

  // The Gekko player number is an in-game/network slot, not a local controller index.
  // Like Dolphin NetPlay, each client maps its first configured local pad to its assigned slot.
  // Using local_player - 1 here made player 2 read GCPad2 even though its controller is GCPad1.
  constexpr int LOCAL_PAD = 0;
  GCPadStatus local_status;
  if (g_manager.local_uses_gc_adapter)
  {
    local_status = GCAdapter::Input(LOCAL_PAD);
  }
  else
  {
    local_status = Pad::GetStatus(LOCAL_PAD);
  }
  if (g_manager.debug_p2_cstick && g_manager.local_player == 2)
    ApplyDebugCStickPattern(&local_status, g_manager.local_input_frame);
  WirePad wire = EncodePad(local_status);

  const bool sampled_start = WirePadHasStart(wire);
  if (sampled_start != g_manager.last_sampled_start)
  {
    if (g_manager.compare_confirmed_ram)
    {
      NOTICE_LOG_FMT(CORE, "GekkoNet input trace: local P{} sampled START {} at input frame {}",
                     g_manager.local_player, sampled_start ? "DOWN" : "UP",
                     g_manager.local_input_frame);
    }
    g_manager.last_sampled_start = sampled_start;
  }

  if (g_manager.last_local_wire)
  {
    bool changed = false;
    for (std::size_t i = 0; i < sizeof(WirePad); ++i)
    {
      if (wire.data[i] != g_manager.last_local_wire->data[i])
      {
        changed = true;
        ++g_manager.perf_local_byte_changes[i];
      }
    }
    if (changed)
      ++g_manager.perf_local_input_changes;
  }
  g_manager.last_local_wire = wire;

  // Keep the raw host sample beside the Gekko frame on which it was submitted. When Gekko later
  // emits the local player's input for an Advance event, this lets diagnostics verify the actual
  // delay behavior instead of merely reporting the configured value.
  g_manager.sampled_local_inputs.push_back(
      {static_cast<s64>(g_manager.local_input_frame), wire});

  gekko_add_local_input(g_manager.session, g_manager.local_handle, &wire);

  if (g_manager.simulated_peer_session && g_manager.simulated_peer_p2_handle >= 0)
  {
    GCPadStatus simulated_p2{};
    if (g_manager.stress_test)
      ApplyStressCStickPattern(&simulated_p2, g_manager.local_input_frame);
    else
      ApplyDebugCStickPattern(&simulated_p2, g_manager.local_input_frame);
    WirePad simulated_wire = EncodePad(simulated_p2);
    gekko_add_local_input(g_manager.simulated_peer_session, g_manager.simulated_peer_p2_handle,
                          &simulated_wire);
  }
  ++g_manager.local_input_frame;
  s_simulated_link_frame = g_manager.local_input_frame;
}

static void PumpSimulatedPeer()
{
  if (!g_manager.simulated_peer_session)
    return;

  gekko_network_poll(g_manager.simulated_peer_session);
  // Once deterministic frame delay is active, advance the hidden protocol peer exactly once per
  // visible real frame. Repeated calls in the 100 us wait loop still poll packets but cannot make
  // the hidden session race ahead and change the requested rollback depth.
  if (g_manager.stress_test && s_stress_frame_delay_active &&
      s_last_stress_peer_update_frame == s_simulated_link_frame)
  {
    return;
  }
  if (g_manager.stress_test && s_stress_frame_delay_active)
    s_last_stress_peer_update_frame = s_simulated_link_frame;

  int event_count = 0;
  GekkoGameEvent** events = gekko_update_session(g_manager.simulated_peer_session, &event_count);
  for (int i = 0; i < event_count; ++i)
  {
    GekkoGameEvent* event = events[i];
    if (!event)
      continue;
    // The hidden peer exists only to exercise GekkoNet's real remote-input path. Its emulation
    // state is the visible Dolphin instance, so acknowledge peer-side state events without making
    // a second, non-authoritative save/load implementation.
    if (event->type == GekkoSaveEvent)
    {
      if (event->data.save.checksum)
        *event->data.save.checksum = 0;
      if (event->data.save.state_len)
        *event->data.save.state_len = 0;
    }
  }
  int session_event_count = 0;
  gekko_session_events(g_manager.simulated_peer_session, &session_event_count);
}

static void LatchInputs(const unsigned char* raw_inputs)
{
  if (!raw_inputs)
    return;

  for (int p = 0; p < g_manager.players; ++p)
  {
    const int handle = g_manager.player_handles[p];
    if (handle >= 0 && handle < g_manager.players)
    {
      WirePad wire{};
      std::memcpy(&wire, raw_inputs + (handle * sizeof(WirePad)), sizeof(WirePad));
      g_manager.latched_pads[p] = DecodePad(wire);
    }
  }
}

static bool InputsEqual(const WirePad& lhs, const WirePad& rhs)
{
  return std::memcmp(&lhs, &rhs, sizeof(WirePad)) == 0;
}

static void MeasureLocalInputDelay(int advance_frame, const std::vector<unsigned char>& inputs)
{
  if (g_manager.local_handle < 0)
    return;

  const std::size_t input_offset =
      static_cast<std::size_t>(g_manager.local_handle) * sizeof(WirePad);
  if (inputs.size() < input_offset + sizeof(WirePad))
  {
    ++g_manager.perf_delay_missing;
    return;
  }

  WirePad applied{};
  std::memcpy(&applied, inputs.data() + input_offset, sizeof(WirePad));

  const s64 expected_sample_frame =
      static_cast<s64>(advance_frame) - g_manager.configured_local_delay;
  if (expected_sample_frame >= 0)
  {
    const auto expected = std::find_if(
        g_manager.sampled_local_inputs.begin(), g_manager.sampled_local_inputs.end(),
        [expected_sample_frame](const SampledLocalInput& sample) {
          return sample.frame == expected_sample_frame;
        });
    if (expected == g_manager.sampled_local_inputs.end())
    {
      ++g_manager.perf_delay_missing;
    }
    else
    {
      ++g_manager.perf_delay_checks;
      if (InputsEqual(applied, expected->input))
        ++g_manager.perf_delay_matches;
      else
        ++g_manager.perf_delay_mismatches;
    }
  }

  // An applied transition can be matched to the newest identical sampled transition. This is an
  // independent observation of the effective delay and will report zero if the host bypasses the
  // configured Gekko delay. It is intentionally transition-based because an unchanged held input
  // cannot reveal how many frames it was delayed.
  if (g_manager.last_applied_local_wire &&
      !InputsEqual(applied, *g_manager.last_applied_local_wire))
  {
    for (std::size_t i = g_manager.sampled_local_inputs.size(); i-- > 1;)
    {
      const SampledLocalInput& sample = g_manager.sampled_local_inputs[i];
      const SampledLocalInput& previous = g_manager.sampled_local_inputs[i - 1];
      if (sample.frame > advance_frame || !InputsEqual(sample.input, applied) ||
          InputsEqual(sample.input, previous.input))
      {
        continue;
      }

      const u64 observed_delay = static_cast<u64>(advance_frame - sample.frame);
      ++g_manager.perf_delay_transitions;
      g_manager.perf_delay_transition_frames += observed_delay;
      if (g_manager.perf_delay_transitions == 1)
        g_manager.perf_delay_transition_min = observed_delay;
      else
        g_manager.perf_delay_transition_min =
            std::min(g_manager.perf_delay_transition_min, observed_delay);
      g_manager.perf_delay_transition_max =
          std::max(g_manager.perf_delay_transition_max, observed_delay);
      break;
    }
  }
  g_manager.last_applied_local_wire = applied;

  constexpr s64 SAMPLE_HISTORY_FRAMES = 64;
  const s64 oldest_needed = static_cast<s64>(advance_frame) - SAMPLE_HISTORY_FRAMES;
  while (g_manager.sampled_local_inputs.size() > 2 &&
         g_manager.sampled_local_inputs[1].frame < oldest_needed)
  {
    g_manager.sampled_local_inputs.pop_front();
  }
}

static void QueueEvents(GekkoGameEvent** events, int event_count)
{
  for (int i = 0; i < event_count; ++i)
  {
    const GekkoGameEvent* event = events[i];
    if (!event)
      continue;

    QueuedEvent queued;
    queued.type = event->type;
    switch (event->type)
    {
    case GekkoSaveEvent:
      queued.frame = event->data.save.frame;
      queued.checksum = event->data.save.checksum;
      queued.state_len = event->data.save.state_len;
      break;
    case GekkoLoadEvent:
      queued.frame = event->data.load.frame;
      break;
    case GekkoAdvanceEvent:
      queued.frame = event->data.adv.frame;
      queued.rolling_back = event->data.adv.rolling_back;
      queued.running_ahead = event->data.adv.running_ahead;
      if (event->data.adv.inputs && event->data.adv.input_len != 0)
      {
        queued.inputs.assign(event->data.adv.inputs,
                             event->data.adv.inputs + event->data.adv.input_len);
      }
      break;
    default:
      break;
    }
    g_manager.pending_events.emplace_back(std::move(queued));
  }
}

static double ElapsedMs(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

static void AddSnapshotPhases(SnapshotPhaseTimings* total, const SnapshotPhaseTimings& sample)
{
  total->state_ms += sample.state_ms;
  total->dirty_pages_ms += sample.dirty_pages_ms;
  total->l1_ms += sample.l1_ms;
  total->jit_ms += sample.jit_ms;
  total->journal_ms += sample.journal_ms;
  total->state_bytes += sample.state_bytes;
  total->changed_blocks += sample.changed_blocks;
  total->changed_cache_lines += sample.changed_cache_lines;
  total->jit_blocks_invalidated += sample.jit_blocks_invalidated;
}

// Native GekkoNet uses frame -1 for the snapshot immediately before frame 0. SnapshotRing uses
// -1 as its empty-slot sentinel, so offset every Gekko frame by one without changing what Orca
// saves or restores.
static s64 SnapshotFrameKey(int gekko_frame)
{
  return static_cast<s64>(gekko_frame) + 1;
}

static void RecordPumpTime(std::chrono::steady_clock::time_point start)
{
  const double elapsed = ElapsedMs(start);
  ++g_manager.perf_pump_count;
  g_manager.perf_pump_ms += elapsed;
  g_manager.perf_pump_max_ms = std::max(g_manager.perf_pump_max_ms, elapsed);
}

static void MaybeLogPerformance(Core::System& system)
{
  constexpr u64 REPORT_FRAMES = 300;
  if (g_manager.perf_real_frames < REPORT_FRAMES)
    return;

  const auto now = std::chrono::steady_clock::now();
  const double wall_ms =
      std::chrono::duration<double, std::milli>(now - g_manager.perf_window_start).count();
  const double fps = wall_ms > 0.0 ? g_manager.perf_real_frames * 1000.0 / wall_ms : 0.0;
  const auto average = [](double total, u64 count) { return count ? total / count : 0.0; };
  const DirtyPages::Counters dirty_pages = DirtyPages::GetCounters();
  const Common::ENet::RollbackDatagramStats udp = Common::ENet::GetRollbackDatagramStats();
  const ResimJitMemoryStats jit_memory = TakeResimJitMemoryStats();
  const std::vector<ResimGuestHotspot> guest_hotspots = TakeResimGuestHotspots(8);
  const CoreTiming::RollbackThrottleStats throttle =
      system.GetCoreTiming().TakeRollbackThrottleStats();

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet perf: {:.1f} fps over {} real frames; save {:.3f}/{:.3f} ms avg/max ({}); "
      "pump {:.3f}/{:.3f} ms ({}); load {:.3f}/{:.3f} ms ({}); replays {}; "
      "replay_exec {:.3f}/{:.3f} ms ({}); replay_jit {:.3f} ms/frame, {:.2f} blocks/frame; "
      "replay_subsystems cpu_jit {:.3f} unaccounted {:.3f} boundary_setup {:.3f} "
      "scheduler {:.3f} fifo {:.3f} vi {:.3f} dsp_audio {:.3f} ios_ipc {:.3f} devices {:.3f} "
      "events_other {:.3f} ms/frame; "
      "replay_save {:.3f}/{:.3f} ms ({}); "
      "burst {:.3f}/{:.3f} ms ({}); burst_parts load {:.3f}/{:.3f} exec {:.3f}/{:.3f} "
      "save {:.3f}/{:.3f} other {:.3f}/{:.3f} ms avg/max; "
      "real_interval {:.3f}/{:.3f} ms ({}); input_changes {} bytes [{},{},{},{},{},{},{},{}]; "
      "delay_measure cfg={} verified={}/{} mismatch={} missing={} observed "
      "{:.2f}/{}/{} frames avg/min/max ({} transitions); "
      "rollback_depth {:.2f}/{} avg/max; RAM dirty/saved/unchanged pages {}/{}/{}; "
      "udp tx/rx/reject {}/{}/{}; ahead={:.2f}; stress_every_3f={}; "
      "stress_rollback_frames={}; "
      "tracked_bitmap_clear={}; full_scan_benchmark={}",
      fps, g_manager.perf_real_frames, average(g_manager.perf_save_ms, g_manager.perf_save_count),
      g_manager.perf_save_max_ms, g_manager.perf_save_count,
      average(g_manager.perf_pump_ms, g_manager.perf_pump_count), g_manager.perf_pump_max_ms,
      g_manager.perf_pump_count, average(g_manager.perf_load_ms, g_manager.perf_load_count),
      g_manager.perf_load_max_ms, g_manager.perf_load_count, g_manager.perf_replay_frames,
      average(g_manager.perf_replay_exec_ms, g_manager.perf_replay_exec_count),
      g_manager.perf_replay_exec_max_ms, g_manager.perf_replay_exec_count,
      average(static_cast<double>(g_manager.perf_replay_jit_nanoseconds) / 1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_jit_blocks),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::GuestCpu)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(std::max(
                  0.0,
                  g_manager.perf_replay_exec_ms -
                      static_cast<double>(std::accumulate(
                          g_manager.perf_replay_events.nanoseconds.begin(),
                          g_manager.perf_replay_events.nanoseconds.end(), u64{0})) /
                          1'000'000.0),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::FrameSetup)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::CoreTiming)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::Fifo)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::VideoInterface)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::DspAudio)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::IosIpc)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::Devices)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::Other)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(g_manager.perf_replay_save_ms, g_manager.perf_replay_save_count),
      g_manager.perf_replay_save_max_ms, g_manager.perf_replay_save_count,
      average(g_manager.perf_rollback_burst_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_burst_max_ms, g_manager.perf_rollback_burst_count,
      average(g_manager.perf_burst_load_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_load_max_ms,
      average(g_manager.perf_burst_exec_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_exec_max_ms,
      average(g_manager.perf_burst_save_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_save_max_ms,
      average(g_manager.perf_burst_other_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_other_max_ms,
      average(g_manager.perf_real_interval_ms, g_manager.perf_real_interval_count),
      g_manager.perf_real_interval_max_ms, g_manager.perf_real_interval_count,
      g_manager.perf_local_input_changes, g_manager.perf_local_byte_changes[0],
      g_manager.perf_local_byte_changes[1], g_manager.perf_local_byte_changes[2],
      g_manager.perf_local_byte_changes[3], g_manager.perf_local_byte_changes[4],
      g_manager.perf_local_byte_changes[5], g_manager.perf_local_byte_changes[6],
      g_manager.perf_local_byte_changes[7],
      g_manager.configured_local_delay, g_manager.perf_delay_matches,
      g_manager.perf_delay_checks, g_manager.perf_delay_mismatches,
      g_manager.perf_delay_missing,
      average(static_cast<double>(g_manager.perf_delay_transition_frames),
              g_manager.perf_delay_transitions),
      g_manager.perf_delay_transition_min, g_manager.perf_delay_transition_max,
      g_manager.perf_delay_transitions,
      average(static_cast<double>(g_manager.perf_rollback_depth_total),
              g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_depth_max,
      dirty_pages.dirty_pages - g_manager.perf_dirty_pages_start.dirty_pages,
      dirty_pages.pages_recorded - g_manager.perf_dirty_pages_start.pages_recorded,
      (dirty_pages.dirty_pages - g_manager.perf_dirty_pages_start.dirty_pages) -
          (dirty_pages.pages_recorded - g_manager.perf_dirty_pages_start.pages_recorded),
      udp.sent, udp.received, udp.rejected, gekko_frames_ahead(g_manager.session),
      g_manager.stress_test, s_stress_packet_delay_frames, g_manager.tracked_bitmap_clear,
      g_manager.full_scan_benchmark);

  NOTICE_LOG_FMT(CORE,
                 "GekkoNet presentation cadence: gaps >20/>25/>33.3/>50 ms = {}/{}/{}/{} of {} "
                 "real frames",
                 g_manager.perf_real_gaps_over_20_ms, g_manager.perf_real_gaps_over_25_ms,
                 g_manager.perf_real_gaps_over_33_ms, g_manager.perf_real_gaps_over_50_ms,
                 g_manager.perf_real_interval_count);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet throttle: normal calls {}; requested {:.3f}/{:.3f} ms avg/max; actual "
      "{:.3f}/{:.3f} ms avg/max; first-after-burst calls {}; requested {:.3f}/{:.3f} ms "
      "avg/max; actual {:.3f}/{:.3f} ms avg/max",
      throttle.normal_calls,
      average(static_cast<double>(throttle.normal_requested_ns) / 1'000'000.0,
              throttle.normal_calls),
      static_cast<double>(throttle.normal_requested_max_ns) / 1'000'000.0,
      average(static_cast<double>(throttle.normal_actual_ns) / 1'000'000.0,
              throttle.normal_calls),
      static_cast<double>(throttle.normal_actual_max_ns) / 1'000'000.0,
      throttle.post_burst_calls,
      average(static_cast<double>(throttle.post_burst_requested_ns) / 1'000'000.0,
              throttle.post_burst_calls),
      static_cast<double>(throttle.post_burst_requested_max_ns) / 1'000'000.0,
      average(static_cast<double>(throttle.post_burst_actual_ns) / 1'000'000.0,
              throttle.post_burst_calls),
      static_cast<double>(throttle.post_burst_actual_max_ns) / 1'000'000.0);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet replay callbacks: cpu_slices {:.2f}, boundary_setups {:.2f}, fifo {:.2f}, "
      "vi {:.2f}, dsp_audio {:.2f}, ios_ipc {:.2f}, devices {:.2f}, events_other {:.2f} "
      "callbacks/frame",
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::GuestCpu)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::FrameSetup)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::Fifo)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::VideoInterface)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::DspAudio)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::IosIpc)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::Devices)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::Other)]),
              g_manager.perf_replay_exec_count));

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet replay JIT memory helpers: reads {:.2f} calls/{:.3f} ms, writes {:.2f} "
      "calls/{:.3f} ms per replay frame",
      average(static_cast<double>(jit_memory.read_calls), g_manager.perf_replay_exec_count),
      average(static_cast<double>(jit_memory.read_nanoseconds) / 1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(jit_memory.write_calls), g_manager.perf_replay_exec_count),
      average(static_cast<double>(jit_memory.write_nanoseconds) / 1'000'000.0,
              g_manager.perf_replay_exec_count));

  const u64 guest_total_nanoseconds =
      g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(ResimEventCategory::GuestCpu)];
  for (const ResimGuestHotspot& hotspot : guest_hotspots)
  {
    const double percent = guest_total_nanoseconds == 0 ?
                               0.0 :
                               100.0 * static_cast<double>(hotspot.nanoseconds) /
                                   static_cast<double>(guest_total_nanoseconds);
    NOTICE_LOG_FMT(
        CORE,
        "GekkoNet replay CPU hotspot: pc={:08x} '{}' {:.3f} ms/frame ({:.1f}% CPU/JIT), "
        "{:.1f} slices/frame, {:.0f} guest cycles/frame",
        hotspot.pc, system.GetPPCSymbolDB().GetDescription(hotspot.pc),
        average(static_cast<double>(hotspot.nanoseconds) / 1'000'000.0,
                g_manager.perf_replay_exec_count),
        percent, average(static_cast<double>(hotspot.slices), g_manager.perf_replay_exec_count),
        average(static_cast<double>(hotspot.cycles), g_manager.perf_replay_exec_count));
  }

  const double save_known_ms = g_manager.perf_save_phases.state_ms +
                               g_manager.perf_save_phases.dirty_pages_ms +
                               g_manager.perf_save_phases.l1_ms +
                               g_manager.perf_save_phases.journal_ms;
  const double load_known_ms = g_manager.perf_load_phases.dirty_pages_ms +
                               g_manager.perf_load_phases.jit_ms +
                               g_manager.perf_load_phases.l1_ms +
                               g_manager.perf_load_phases.state_ms +
                               g_manager.perf_load_phases.journal_ms;
  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet snapshot phases: save state {:.3f} dirty {:.3f} l1 {:.3f} journal {:.3f} "
      "other {:.3f} ms, state {:.1f} KiB; load dirty {:.3f} jit {:.3f} l1 {:.3f} state "
      "{:.3f} journal {:.3f} other {:.3f} ms, changed {:.1f} pages/{:.1f} cache lines, "
      "invalidated {:.1f} JIT blocks",
      average(g_manager.perf_save_phases.state_ms, g_manager.perf_save_count),
      average(g_manager.perf_save_phases.dirty_pages_ms, g_manager.perf_save_count),
      average(g_manager.perf_save_phases.l1_ms, g_manager.perf_save_count),
      average(g_manager.perf_save_phases.journal_ms, g_manager.perf_save_count),
      average(std::max(0.0, g_manager.perf_save_ms - save_known_ms),
              g_manager.perf_save_count),
      average(static_cast<double>(g_manager.perf_save_phases.state_bytes),
              g_manager.perf_save_count) /
          1024.0,
      average(g_manager.perf_load_phases.dirty_pages_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.jit_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.l1_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.state_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.journal_ms, g_manager.perf_load_count),
      average(std::max(0.0, g_manager.perf_load_ms - load_known_ms),
              g_manager.perf_load_count),
      average(static_cast<double>(g_manager.perf_load_phases.changed_blocks),
              g_manager.perf_load_count),
      average(static_cast<double>(g_manager.perf_load_phases.changed_cache_lines),
              g_manager.perf_load_count),
      average(static_cast<double>(g_manager.perf_load_phases.jit_blocks_invalidated),
              g_manager.perf_load_count));

  if (g_manager.compare_confirmed_ram)
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback restore diagnostic: verified {} snapshot loads; {} restore "
                   "mismatches in this window",
                   g_manager.perf_restore_checks, g_manager.perf_restore_mismatches);
  }

  g_manager.perf_window_start = now;
  g_manager.perf_dirty_pages_start = dirty_pages;
  g_manager.perf_real_frames = 0;
  g_manager.perf_replay_frames = 0;
  g_manager.perf_save_count = 0;
  g_manager.perf_load_count = 0;
  g_manager.perf_pump_count = 0;
  g_manager.perf_save_ms = 0.0;
  g_manager.perf_save_max_ms = 0.0;
  g_manager.perf_load_ms = 0.0;
  g_manager.perf_load_max_ms = 0.0;
  g_manager.perf_pump_ms = 0.0;
  g_manager.perf_pump_max_ms = 0.0;
  g_manager.perf_save_phases = {};
  g_manager.perf_load_phases = {};
  g_manager.perf_local_input_changes = 0;
  g_manager.perf_local_byte_changes.fill(0);
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  g_manager.perf_replay_exec_count = 0;
  g_manager.perf_replay_exec_ms = 0.0;
  g_manager.perf_replay_exec_max_ms = 0.0;
  g_manager.perf_replay_jit_blocks = 0;
  g_manager.perf_replay_jit_nanoseconds = 0;
  g_manager.perf_replay_events = {};
  g_manager.perf_replay_save_count = 0;
  g_manager.perf_replay_save_ms = 0.0;
  g_manager.perf_replay_save_max_ms = 0.0;
  g_manager.perf_rollback_burst_count = 0;
  g_manager.perf_rollback_burst_ms = 0.0;
  g_manager.perf_rollback_burst_max_ms = 0.0;
  g_manager.perf_burst_load_ms = 0.0;
  g_manager.perf_burst_load_max_ms = 0.0;
  g_manager.perf_burst_exec_ms = 0.0;
  g_manager.perf_burst_exec_max_ms = 0.0;
  g_manager.perf_burst_save_ms = 0.0;
  g_manager.perf_burst_save_max_ms = 0.0;
  g_manager.perf_burst_other_ms = 0.0;
  g_manager.perf_burst_other_max_ms = 0.0;
  g_manager.perf_rollback_depth_total = 0;
  g_manager.perf_rollback_depth_max = 0;
  g_manager.perf_real_interval_count = 0;
  g_manager.perf_real_interval_ms = 0.0;
  g_manager.perf_real_interval_max_ms = 0.0;
  g_manager.perf_real_gaps_over_20_ms = 0;
  g_manager.perf_real_gaps_over_25_ms = 0;
  g_manager.perf_real_gaps_over_33_ms = 0;
  g_manager.perf_real_gaps_over_50_ms = 0;
  g_manager.perf_restore_checks = 0;
  g_manager.perf_restore_mismatches = 0;
}

// Consumes state operations up to one advance. Returning true means that the caller must return
// to the CPU so the selected frame can actually run. GekkoNet can emit a complete rollback as one
// batch (load, several replay advances/saves, then a real advance); Dolphin must spread that batch
// across real frame boundaries instead of merely relatching all of its inputs in one callback.
static bool PrepareQueuedFrame(Core::System& system)
{
  PROFILER_SCOPE_FUNCTION();
  while (!g_manager.pending_events.empty())
  {
    QueuedEvent event = std::move(g_manager.pending_events.front());
    g_manager.pending_events.pop_front();
    RecordDiagnosticEvent(system, event);

    switch (event.type)
    {
    case GekkoSaveEvent:
      if (event.frame >= -1 && g_manager.ring)
      {
        const auto start = std::chrono::steady_clock::now();
        g_manager.ring->Save(system, SnapshotFrameKey(event.frame));
        LogStartFrameStateDigest(system, event.frame);
        RecordRamDigest(system, event.frame);
        LogSparseStateCheckpoint(system, event.frame);
        const double elapsed = ElapsedMs(start);
        AddSnapshotPhases(&g_manager.perf_save_phases, g_manager.ring->GetLastSaveTimings());
        ++g_manager.perf_save_count;
        g_manager.perf_save_ms += elapsed;
        g_manager.perf_save_max_ms = std::max(g_manager.perf_save_max_ms, elapsed);
        if (Rollback::IsResimulating() && g_manager.rollback_burst_start)
        {
          ++g_manager.perf_replay_save_count;
          g_manager.perf_replay_save_ms += elapsed;
          g_manager.perf_replay_save_max_ms =
              std::max(g_manager.perf_replay_save_max_ms, elapsed);
          g_manager.current_burst_save_ms += elapsed;
        }
        if (event.checksum)
          *event.checksum = 0;
        if (event.state_len)
          *event.state_len = 0;
      }
      break;

    case GekkoLoadEvent:
      if (event.frame >= -1 && g_manager.ring)
      {
        DEBUG_LOG_FMT(CORE, "GekkoNet: Loading rollback frame {}", event.frame);
        if (!Rollback::IsResimulating() && !g_manager.throttle_reference_before_load)
        {
          g_manager.throttle_reference_before_load = system.GetCoreTiming().GetThrottleReference();
        }
        if (!g_manager.rollback_burst_start)
        {
          g_manager.rollback_burst_start = std::chrono::steady_clock::now();
          g_manager.current_rollback_replays = 0;
          g_manager.current_burst_load_ms = 0.0;
          g_manager.current_burst_exec_ms = 0.0;
          g_manager.current_burst_save_ms = 0.0;
          g_manager.current_burst_jit_blocks_invalidated = 0;
        }
        const auto start = std::chrono::steady_clock::now();
        if (!g_manager.ring->Load(system, SnapshotFrameKey(event.frame)))
        {
          ERROR_LOG_FMT(CORE, "GekkoNet: Failed to restore rollback frame {}", event.frame);
          g_manager.stop_requested.store(true, std::memory_order_relaxed);
          return false;
        }
        VerifyRestoredRam(system, event.frame);
        if (g_manager.stress_test && event.frame < 10)
        {
          const auto& ppc = system.GetPPCState();
          NOTICE_LOG_FMT(CORE,
                         "GekkoNet trace: load event {} returned (pc={:08x}, npc={:08x}, "
                         "downcount={}, ticks={})",
                         event.frame, ppc.pc, ppc.npc, ppc.downcount,
                         system.GetCoreTiming().GetTicks());
        }
        const double elapsed = ElapsedMs(start);
        const SnapshotPhaseTimings& load_timings = g_manager.ring->GetLastLoadTimings();
        AddSnapshotPhases(&g_manager.perf_load_phases, load_timings);
        g_manager.current_burst_jit_blocks_invalidated += load_timings.jit_blocks_invalidated;
        ++g_manager.perf_load_count;
        g_manager.perf_load_ms += elapsed;
        g_manager.perf_load_max_ms = std::max(g_manager.perf_load_max_ms, elapsed);
        g_manager.current_burst_load_ms += elapsed;
      }
      break;

    case GekkoAdvanceEvent:
    {
      const bool resimulating = event.rolling_back || event.running_ahead;
      std::string start_players;
      std::pair<u32, u32> si0_before{};
      std::pair<u32, u32> si1_before{};
      if (g_manager.compare_confirmed_ram)
      {
        g_manager.last_advance_input_hash =
            XXH3_64bits(event.inputs.data(), event.inputs.size());
        for (int p = 0; p < g_manager.players; ++p)
        {
          const int handle = g_manager.player_handles[p];
          if (handle < 0)
            continue;
          const std::size_t offset = static_cast<std::size_t>(handle) * sizeof(WirePad);
          if (event.inputs.size() < offset + sizeof(WirePad))
            continue;
          WirePad wire{};
          std::memcpy(&wire, event.inputs.data() + offset, sizeof(wire));
          if (WirePadHasStart(wire))
            start_players += fmt::format("{}P{}", start_players.empty() ? "" : ",", p + 1);
        }
        si0_before = system.GetSerialInterface().GetInputRegisters(0);
        si1_before = system.GetSerialInterface().GetInputRegisters(1);
      }
      g_manager.last_advance_has_start = !start_players.empty();
      if (!resimulating)
        MeasureLocalInputDelay(event.frame, event.inputs);
      LatchInputs(event.inputs.data());
      Rollback::SetResimulating(resimulating);
      if (!resimulating && g_manager.throttle_reference_before_load)
      {
        system.GetCoreTiming().SetThrottleReference(*g_manager.throttle_reference_before_load);
        g_manager.throttle_reference_before_load.reset();
      }
      const auto advance_now = std::chrono::steady_clock::now();
      if (!resimulating && g_manager.rollback_burst_start)
      {
        const auto burst_duration = advance_now - *g_manager.rollback_burst_start;
        const double elapsed = std::chrono::duration<double, std::milli>(burst_duration).count();
        Rollback::RecordRollbackBurst(static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(burst_duration).count()));
        ++g_manager.perf_rollback_burst_count;
        g_manager.perf_rollback_burst_ms += elapsed;
        g_manager.perf_rollback_burst_max_ms =
            std::max(g_manager.perf_rollback_burst_max_ms, elapsed);
        const double other_ms =
            std::max(0.0, elapsed - g_manager.current_burst_load_ms -
                              g_manager.current_burst_exec_ms - g_manager.current_burst_save_ms);
        g_manager.perf_burst_load_ms += g_manager.current_burst_load_ms;
        g_manager.perf_burst_load_max_ms =
            std::max(g_manager.perf_burst_load_max_ms, g_manager.current_burst_load_ms);
        g_manager.perf_burst_exec_ms += g_manager.current_burst_exec_ms;
        g_manager.perf_burst_exec_max_ms =
            std::max(g_manager.perf_burst_exec_max_ms, g_manager.current_burst_exec_ms);
        g_manager.perf_burst_save_ms += g_manager.current_burst_save_ms;
        g_manager.perf_burst_save_max_ms =
            std::max(g_manager.perf_burst_save_max_ms, g_manager.current_burst_save_ms);
        g_manager.perf_burst_other_ms += other_ms;
        g_manager.perf_burst_other_max_ms =
            std::max(g_manager.perf_burst_other_max_ms, other_ms);
        g_manager.perf_rollback_depth_total += g_manager.current_rollback_replays;
        g_manager.perf_rollback_depth_max =
            std::max(g_manager.perf_rollback_depth_max, g_manager.current_rollback_replays);
        g_manager.current_rollback_replays = 0;
        g_manager.rollback_burst_start.reset();
      }
      if (resimulating)
      {
        ++g_manager.perf_replay_frames;
        ++g_manager.current_rollback_replays;
      }
      else
      {
        if (g_manager.last_real_advance)
        {
          const double interval =
              std::chrono::duration<double, std::milli>(advance_now - *g_manager.last_real_advance)
                  .count();
          ++g_manager.perf_real_interval_count;
          g_manager.perf_real_interval_ms += interval;
          g_manager.perf_real_interval_max_ms =
              std::max(g_manager.perf_real_interval_max_ms, interval);
          g_manager.perf_real_gaps_over_20_ms += interval > 20.0;
          g_manager.perf_real_gaps_over_25_ms += interval > 25.0;
          g_manager.perf_real_gaps_over_33_ms += interval > (100.0 / 3.0);
          g_manager.perf_real_gaps_over_50_ms += interval > 50.0;
        }
        g_manager.last_real_advance = advance_now;
        if (g_manager.perf_real_frames == 0)
          g_manager.perf_window_start = std::chrono::steady_clock::now();
        ++g_manager.perf_real_frames;
      }
      g_manager.frame_execution_start = advance_now;
      g_manager.frame_execution_resim = resimulating;
      g_manager.frame_execution_gekko_frame = event.frame;
      g_manager.frame_execution_jit_start = GetResimJitCompileStats();
      g_manager.frame_execution_event_start = GetResimEventStats();
      const auto frame_setup_start =
          resimulating ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      // VI boundaries select the next rollback input before VI's normally scheduled SI poll.
      // Relatching here would expose it immediately at a JIT-dependent guest instruction.
      if (g_manager.frame_boundary == FrameBoundary::BrawlHook)
        system.GetSerialInterface().RelatchInputs();
      if (g_manager.compare_confirmed_ram && !start_players.empty())
      {
        const auto si0_after = system.GetSerialInterface().GetInputRegisters(0);
        const auto si1_after = system.GetSerialInterface().GetInputRegisters(1);
        const auto& ppc = system.GetPPCState();
        NOTICE_LOG_FMT(
            CORE,
            "GekkoNet input trace: advance frame {} START [{}], rollback={}, runahead={}, "
            "hash={:016x}, ticks={}, timebase={}, vi_halfline={}, pc={:08x}, npc={:08x}, "
            "si0={:08x}/{:08x}->{:08x}/{:08x}, si1={:08x}/{:08x}->{:08x}/{:08x}",
            event.frame, start_players, event.rolling_back, event.running_ahead,
            g_manager.last_advance_input_hash, system.GetCoreTiming().GetTicks(),
            system.GetPowerPC().ReadFullTimeBaseValue(),
            system.GetVideoInterface().GetHalfLineCount(), ppc.pc, ppc.npc, si0_before.first,
            si0_before.second, si0_after.first, si0_after.second, si1_before.first,
            si1_before.second, si1_after.first, si1_after.second);
      }
      if (g_manager.stress_test && event.frame < 10)
      {
        NOTICE_LOG_FMT(CORE,
                       "GekkoNet trace: advance event {} returning to CPU (rollback={}, "
                       "runahead={})",
                       event.frame, event.rolling_back, event.running_ahead);
      }
      if (event.rolling_back || event.running_ahead)
      {
        DEBUG_LOG_FMT(CORE, "GekkoNet: Replaying frame {} (rollback={}, runahead={})", event.frame,
                      event.rolling_back, event.running_ahead);
      }
      if (resimulating)
      {
        RecordResimEvent(
            ResimEventCategory::FrameSetup,
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - frame_setup_start)
                                 .count()));
      }
      return true;
    }

    default:
      break;
    }
  }
  return false;
}

static void ProcessFrameBoundary(const Core::CPUThreadGuard& guard, FrameBoundary boundary)
{
  PROFILER_SCOPE_FUNCTION();
  auto& system = guard.GetSystem();
  HostFloatScope float_scope(system.GetPPCState());

  if (!g_manager.active.load(std::memory_order_acquire) || g_manager.frame_boundary != boundary)
    return;

  if (boundary == FrameBoundary::BrawlHook)
  {
    // A launcher installs this hook before Brawl is in RAM. Until the expected instruction is live,
    // whatever happens to execute at this address is not Brawl's frame boundary.
    const u32 opcode = system.GetMemory().Read_U32(BRAWL_FRAME_HOOK_ADDR & 0x1FFFFFFF);
    if (opcode != BRAWL_EXPECTED_OPCODE)
      return;

    // Inside a JIT block PC and NPC are stale. Pin both to the boundary so snapshots resume at the
    // hook. This is a Start hook, so the original instruction executes normally after this returns.
    auto& ppc_state = system.GetPPCState();
    ppc_state.pc = BRAWL_FRAME_HOOK_ADDR;
    ppc_state.npc = BRAWL_FRAME_HOOK_ADDR;
  }

  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.session)
    return;

  if (g_manager.frame_execution_start)
  {
    const double elapsed = ElapsedMs(*g_manager.frame_execution_start);
    if (g_manager.frame_execution_resim)
    {
      const ResimJitCompileStats jit_now = GetResimJitCompileStats();
      const u64 jit_blocks = jit_now.blocks - g_manager.frame_execution_jit_start.blocks;
      const u64 jit_nanoseconds =
          jit_now.nanoseconds - g_manager.frame_execution_jit_start.nanoseconds;
      const ResimEventStats event_now = GetResimEventStats();
      ++g_manager.perf_replay_exec_count;
      g_manager.perf_replay_exec_ms += elapsed;
      g_manager.perf_replay_exec_max_ms = std::max(g_manager.perf_replay_exec_max_ms, elapsed);
      g_manager.perf_replay_jit_blocks += jit_blocks;
      g_manager.perf_replay_jit_nanoseconds += jit_nanoseconds;
      for (std::size_t i = 0; i < g_manager.perf_replay_events.nanoseconds.size(); ++i)
      {
        g_manager.perf_replay_events.nanoseconds[i] +=
            event_now.nanoseconds[i] - g_manager.frame_execution_event_start.nanoseconds[i];
        g_manager.perf_replay_events.callbacks[i] +=
            event_now.callbacks[i] - g_manager.frame_execution_event_start.callbacks[i];
      }
      if (g_manager.rollback_burst_start)
        g_manager.current_burst_exec_ms += elapsed;
      if (elapsed >= 10.0)
      {
        NOTICE_LOG_FMT(CORE,
                       "GekkoNet replay spike: frame {} exec {:.3f} ms, JIT compiled {} blocks in "
                       "{:.3f} ms after invalidating {} blocks in this burst",
                       g_manager.frame_execution_gekko_frame, elapsed, jit_blocks,
                       static_cast<double>(jit_nanoseconds) / 1'000'000.0,
                       g_manager.current_burst_jit_blocks_invalidated);
      }
    }
    g_manager.frame_execution_start.reset();
  }

  MaybeLogPerformance(system);

  // A rollback batch is replayed one emulated frame at a time. State saves left after the prior
  // advance are intentionally handled now, after that frame has completed.
  if (PrepareQueuedFrame(system))
    return;

  // Only publish confirmed hashes after the previous Gekko event batch has completely drained.
  // A network poll can confirm an input before HandleRollback has emitted and executed the
  // correction, so publishing immediately after polling would compare stale predicted RAM.
  CompareSettledRamDigests();
  SendConfirmedRamDigests();
  if (DumpFirstRamMismatch(system))
    return;

  // Submit before polling so this boundary's inputs are eligible to be sent immediately. The
  // simulated peer uses the same GekkoNet path, with packets released after the configured delay.
  SubmitLocalInput();
  PumpSimulatedPeer();
  gekko_network_poll(g_manager.session);

  // A peer mismatch notification can arrive during the poll above. Restore and dump immediately
  // at this safe boundary, then pause instead of waiting for another Gekko advance.
  if (DumpFirstRamMismatch(system))
    return;

  // Symmetric timesync pacing (RMG-K parameters). Tight deadzone + fast lerp keeps
  // framesAhead near zero so both players share prediction/rollback load.
  static constexpr double kSymDeadzone = 0.20;
  static constexpr double kSymStrength = 0.015;
  static constexpr double kSymMinScale = 0.97;
  static constexpr double kSymMaxScale = 1.03;
  static constexpr double kSymLerp = 0.35;

  const float frames_ahead = gekko_frames_ahead(g_manager.session);

  double newTarget = 1.0;
  if (frames_ahead >= static_cast<float>(kSymDeadzone) ||
      frames_ahead <= -static_cast<float>(kSymDeadzone))
  {
    newTarget = 1.0 - (static_cast<double>(frames_ahead) * kSymStrength);
    newTarget = std::clamp(newTarget, kSymMinScale, kSymMaxScale);
  }
  g_manager.target_scale = newTarget;

  g_manager.speed_scale += (g_manager.target_scale - g_manager.speed_scale) * kSymLerp;
  Core::System::GetInstance().GetCoreTiming().SetTimesyncScale(
      static_cast<float>(g_manager.speed_scale));

  static u32 s_boundary_ticks = 0;
  if (++s_boundary_ticks % 60 == 1 && g_manager.compare_confirmed_ram)
  {
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet: Frame boundary tick #{}, boundary={}, local_player={}, ahead={:.2f}, "
                   "target={:.4f}, scale={:.4f}",
                   s_boundary_ticks, FrameBoundaryName(boundary), g_manager.local_player,
                   frames_ahead, g_manager.target_scale, g_manager.speed_scale);
  }

  // Run GekkoNet event pump - must wait for an advance event each frame.
  const auto pump_start = std::chrono::steady_clock::now();
  for (;;)
  {
    if (g_manager.stop_requested.load(std::memory_order_relaxed))
      return;

    int event_count = 0;
    GekkoGameEvent** events = gekko_update_session(g_manager.session, &event_count);

    // Check session lifecycle events
    int s_count = 0;
    GekkoSessionEvent** s_events = gekko_session_events(g_manager.session, &s_count);
    for (int s = 0; s < s_count; ++s)
    {
      const auto* sev = s_events[s];
      if (!sev)
        continue;
      switch (sev->type)
      {
      case GekkoPlayerSyncing:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Player handle {} syncing ({}/{})", sev->data.syncing.handle,
                       sev->data.syncing.current, sev->data.syncing.max);
        break;
      case GekkoPlayerConnected:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Player handle {} connected!", sev->data.connected.handle);
        break;
      case GekkoSessionStarted:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Session started! Rollback active.");
        if (g_manager.stress_test && !s_stress_frame_delay_active)
        {
          s_stress_frame_delay_active = true;
          s_last_stress_peer_update_frame = 0;
          NOTICE_LOG_FMT(CORE,
                         "GekkoNet benchmark: deterministic {}-frame packets active; Player 2 "
                         "changes input every 3 frames",
                         s_stress_packet_delay_frames);
        }
        break;
      default:
        break;
      }
    }

    QueueEvents(events, event_count);
    if (PrepareQueuedFrame(system))
    {
      RecordPumpTime(pump_start);
      return;
    }

    // No advance yet - poll network and retry.
    // A sub-millisecond std::this_thread::sleep_for can consume a full scheduler quantum on
    // Windows. That turns a brief GekkoNet wait into a lost emulated frame. Dolphin's precision
    // timer spins for this short interval and preserves the intended 100 us polling cadence.
    g_manager.wait_timer.SleepUntil(Clock::now() + std::chrono::microseconds(WAIT_SLEEP_US));
    PumpSimulatedPeer();
    gekko_network_poll(g_manager.session);
  }
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  ProcessFrameBoundary(guard, FrameBoundary::BrawlHook);
  if (IsGekkoSessionActive() && g_manager.frame_boundary == FrameBoundary::BrawlHook &&
      !IsResimulating())
  {
    auto& core_timing = guard.GetSystem().GetCoreTiming();
    core_timing.ThrottleForRollbackBoundary(static_cast<s64>(core_timing.GetTicks()));
  }
}

void SignalVIBoundary(FrameBoundary boundary)
{
  if (g_manager.active.load(std::memory_order_acquire) && g_manager.frame_boundary == boundary)
  {
    g_manager.precise_vi_tail.store(false, std::memory_order_release);
    g_manager.vi_boundary_pending.store(true, std::memory_order_release);
  }
}

bool IsVIBoundaryPending()
{
  return g_manager.vi_boundary_pending.load(std::memory_order_acquire);
}

void UpdatePreciseVITailArm(bool next_is_begin_field, bool next_is_end_field,
                            bool next_is_new_field)
{
  const bool active = g_manager.active.load(std::memory_order_acquire);
  const bool selected =
      (g_manager.frame_boundary == FrameBoundary::VIBeginField && next_is_begin_field) ||
      (g_manager.frame_boundary == FrameBoundary::VIEndField && next_is_end_field) ||
      (g_manager.frame_boundary == FrameBoundary::VINewField && next_is_new_field);
  g_manager.precise_vi_tail.store(active && selected, std::memory_order_release);
}

bool IsPreciseVITailArmed()
{
  return g_manager.precise_vi_tail.load(std::memory_order_acquire);
}

bool IsDeterminismDiagnosticsEnabled()
{
  return g_manager.compare_confirmed_ram;
}

void RunPendingVIBoundary(Core::System& system)
{
  if (!g_manager.vi_boundary_pending.exchange(false, std::memory_order_acq_rel))
    return;

  // The first VI event can run from the JIT's initial CoreTiming::Advance before a guest block has
  // established NPC. Such a snapshot serializes successfully but cannot resume after a load.
  // Wait for the first genuine dispatcher boundary instead of exposing an invalid frame -1.
  const auto& initial_ppc = system.GetPPCState();
  if (initial_ppc.npc == 0)
  {
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet: Ignoring pre-execution VI boundary (pc={:08x}, npc={:08x})",
                   initial_ppc.pc, initial_ppc.npc);
    return;
  }

  const Core::CPUThreadGuard guard(system);
  ProcessFrameBoundary(guard, g_manager.frame_boundary);
  if (IsGekkoSessionActive() && !IsResimulating())
  {
    auto& core_timing = system.GetCoreTiming();
    core_timing.ThrottleForRollbackBoundary(static_cast<s64>(core_timing.GetTicks()));
  }
  static u32 s_early_returns = 0;
  if (g_manager.stress_test && s_early_returns++ < 16)
  {
    const auto& ppc = system.GetPPCState();
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet trace: VI handler returned (pc={:08x}, npc={:08x}, downcount={}, "
                   "ticks={})",
                   ppc.pc, ppc.npc, ppc.downcount, system.GetCoreTiming().GetTicks());
  }
}

}  // namespace Rollback
