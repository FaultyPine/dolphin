#pragma once

#include <random>
#include "Common/CommonTypes.h"
#include "Common/Thread.h"
#include "Core/Brawlback/Netplay/Netplay.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include <unordered_map>
#include <vector>

#include "Core/Brawlback/BrawlbackUtility.h"

#pragma warning( push )
#pragma warning( disable : 4946 )
#include "Core/Brawlback/include/json.hpp"
#pragma warning( pop )

using json = nlohmann::json;

// NOTE: Heavily derived from Slippi's Matchmaking class/logic.

class Matchmaking
{
public:
  Matchmaking(UserInfo user);
  ~Matchmaking();

  enum OnlinePlayMode
  {
    RANKED = 0,
    UNRANKED = 1,
    DIRECT = 2,
    TEAMS = 3,
  };

  enum ProcessState
  {
    IDLE,
    INITIALIZING,
    MATCHMAKING,
    OPPONENT_CONNECTING,
    CONNECTION_SUCCESS,
    ERROR_ENCOUNTERED,
  };
  enum class ConnectStatus
  {
    NET_CONNECT_STATUS_UNSET,
    NET_CONNECT_STATUS_INITIATED,
    NET_CONNECT_STATUS_CONNECTED,
    NET_CONNECT_STATUS_FAILED,
    NET_CONNECT_STATUS_DISCONNECTED,
  };

  struct MatchSearchSettings
  {
    OnlinePlayMode mode = OnlinePlayMode::UNRANKED;
    std::string connectCode = "";
  };

  void FindMatch(MatchSearchSettings settings);
  void MatchmakeThread();
  ProcessState GetMatchmakeState();
  void SetMatchmakeState(ProcessState state);
  bool IsSearching();
  bool IsHost();
  u16 GetLocalPort();

  std::vector<std::string> GetRemoteIPAddresses();
  std::vector<u16> GetRemotePorts();

  // std::unique_ptr<NetplayClient> GetNetplayClient();
  std::string GetErrorMessage();
  int LocalPlayerIndex();
  std::vector<UserInfo> GetPlayerInfo();
  std::string GetPlayerName(u8 port);
  std::vector<u16> GetStages();
  u16 GetRandomStage();
  u8 RemotePlayerCount();
  static bool IsFixedRulesMode(OnlinePlayMode mode);

protected:
  const std::string MM_HOST_DEV = "lylat.gg";
  const std::string MM_HOST_PROD = "lylat.gg";
  const u16 MM_PORT = 43113;
  std::string MM_HOST = "";

  UserInfo m_user;

  ENetHost* m_client;
  ENetPeer* m_server;

  std::default_random_engine generator;

  bool isMmConnected = false;

  std::thread m_matchmakeThread;

  MatchSearchSettings m_searchSettings;

  ProcessState m_state;
  std::string m_errorMsg = "";

  int m_isSwapAttempt = false;

  int m_hostPort;
  int m_localPlayerIndex;
  std::vector<std::string> m_remoteIps;
  std::vector<UserInfo> m_playerInfo;
  std::vector<u16> m_allowedStages;
  bool m_joinedLobby;
  bool m_isHost;

  // std::unique_ptr<NetplayClient> m_netplayClient;

  const std::unordered_map<ProcessState, bool> searchingStates = {
      {ProcessState::INITIALIZING, true},
      {ProcessState::MATCHMAKING, true},
      {ProcessState::OPPONENT_CONNECTING, true},
  };

  std::string getDefaultMMHost();
  std::string getMexMMHost();
  std::string getMMHostForSearchMode();

  void disconnectFromServer();
  void terminateMmConnection();
  void sendMessage(json msg);
  int receiveMessage(json& msg, int maxAttempts);
  //void sendHolePunchMsg(std::string remoteIp, u16 remotePort, u16 localPort);
  void startMatchmaking();
  void handleMatchmaking();
  void handleConnecting();
};
