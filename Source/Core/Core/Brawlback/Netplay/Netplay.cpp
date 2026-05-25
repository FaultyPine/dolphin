#include "Core/Brawlback/Netplay/Netplay.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <thread>
#include "Common/CommonTypes.h"
#include "Common/ENet.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "VideoCommon/VideoConfig.h"
#include "Core/HW/EXI/EXIBrawlback.h"

BrawlbackNetplay::BrawlbackNetplay() {

}
BrawlbackNetplay::~BrawlbackNetplay() {
    
}



void BrawlbackNetplay::SendAsync(std::unique_ptr<BrawlbackNetPacket> packet, ENetHost* host) {
    {
        std::lock_guard<std::recursive_mutex> lock(async_send_packet_mutex);
        async_queue.push_back(std::move(packet));
    }
    Common::ENet::WakeupThread(host);
}

void BrawlbackNetplay::BroadcastPacket(const sf::Packet& packet, int enet_flag, ENetHost* server) {
    ENetPacket* p = enet_packet_create(packet.getData(), packet.getDataSize(), enet_flag);
    enet_host_broadcast(server, 0, p);
}

void BrawlbackNetplay::FlushAsyncQueue(ENetHost* server) {
    while (!async_queue.empty())
    {
        BrawlbackNetPacket* packet = async_queue.front().get();
        BroadcastPacket(packet->first, packet->second, server);
        async_queue.pop_front();
    }
}

void BrawlbackNetplay::BroadcastPlayerFrameData(ENetHost* server, Match::PlayerFrameData* framedata) {
    // send framedata to all peers
    sf::Packet frame_data_packet = sf::Packet();

    // append cmd byte
    u8 frame_data_cmd = NetPacketCommand::CMD_FRAME_DATA;
    frame_data_packet.append(&frame_data_cmd, sizeof(u8));

    // append framedata
    frame_data_packet.append(framedata, sizeof(Match::PlayerFrameData));

    std::pair<sf::Packet, int> pckt_content = std::make_pair(frame_data_packet, ENET_PACKET_FLAG_UNSEQUENCED);
    std::unique_ptr<BrawlbackNetPacket> pckt = std::make_unique<BrawlbackNetPacket>(pckt_content);
    this->SendAsync(std::move(pckt), server);
}

void BrawlbackNetplay::BroadcastGameSettings(ENetHost* server, Match::GameSettings* settings) {
    sf::Packet settingsPckt = sf::Packet();
    u8 cmd_byte = NetPacketCommand::CMD_GAME_SETTINGS;
    settingsPckt.append(&cmd_byte, sizeof(cmd_byte));
    settingsPckt.append(settings, sizeof(Match::GameSettings));

    this->BroadcastPacket(settingsPckt, ENET_PACKET_FLAG_RELIABLE, server);
    INFO_LOG_FMT(BRAWLBACK, "Sent game settings data packet\n");
}


void BrawlbackNetplay::BroadcastPlayerFrameDataWithPastFrames(ENetHost* server, const std::vector<Match::PlayerFrameData*>& framedatas) {
    sf::Packet frame_data_packet = sf::Packet();

    // append cmd byte
    u8 frame_data_cmd = NetPacketCommand::CMD_FRAME_DATA;
    frame_data_packet.append(&frame_data_cmd, sizeof(u8));

    // append number of framedatas that are in this packet
    u8 sizeofFramedatas = (u8)framedatas.size();
    frame_data_packet.append(&sizeofFramedatas, sizeof(sizeofFramedatas));

    // append framedata
    for (Match::PlayerFrameData* framedata : framedatas) {
        frame_data_packet.append(framedata, sizeof(Match::PlayerFrameData));
    }

    //INFO_LOG(BRAWLBACK, "Sending input packet of size: %u   num inputs: %u\n", frame_data_packet.getDataSize(), framedatas.size());

    std::pair<sf::Packet, int> pckt_content = std::make_pair(frame_data_packet, ENET_PACKET_FLAG_UNSEQUENCED);
    std::unique_ptr<BrawlbackNetPacket> pckt = std::make_unique<BrawlbackNetPacket>(pckt_content);
    this->SendAsync(std::move(pckt), server);
}