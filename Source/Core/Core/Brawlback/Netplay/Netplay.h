#pragma once

#include <SFML/Network/Packet.hpp>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>
#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayProto.h"
#include "InputCommon/GCPadStatus.h"
#include "Core/Brawlback/BrawlbackUtility.h"
using namespace Brawlback;

//                packet      enet flag
typedef std::pair<sf::Packet, int> BrawlbackNetPacket;

struct FrameTiming {
    int frame;
    s64 timeUs;
};
struct FrameAck {
    int frame;
    u8 playerIdx;
};

class BrawlbackNetplay {

public:
    BrawlbackNetplay();
    ~BrawlbackNetplay();

    void SendAsync(std::unique_ptr<BrawlbackNetPacket> packet, ENetHost* host);
    void BroadcastPacket(const sf::Packet& packet, int enet_flag, ENetHost* server);
    void FlushAsyncQueue(ENetHost* server);

    // sends FrameData to all peers (async/udp)
    void BroadcastPlayerFrameData(ENetHost* server, Match::PlayerFrameData* framedata);
    // sends GameSettings to all peers (sync/tcp)
    void BroadcastGameSettings(ENetHost* server, Match::GameSettings* settings);

    void BroadcastPlayerFrameDataWithPastFrames(ENetHost* server, const std::vector<Match::PlayerFrameData*>& framedatas);

private:

    std::deque<std::unique_ptr<BrawlbackNetPacket>> async_queue = {};
    std::recursive_mutex async_send_packet_mutex;

};

