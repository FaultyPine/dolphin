#include "Core/Brawlback/Netplay/Matchmaking.h"
#include "Common/Common.h"
#include "Common/ENet.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/ConfigManager.h"
#include "Common/Timer.h"
#include <string>
#include <vector>

#if defined __linux__ && HAVE_ALSA
#elif defined __APPLE__
#include <arpa/inet.h>
#include <netdb.h>
#elif defined _WIN32
#endif

// NOTE: Heavily derived from Slippi's Matchmaking class/logic.

const std::string scm_slippi_semver_str = "Brawlback - dev";

class MmMessageType
{
  public:
	static std::string CREATE_TICKET;
	static std::string CREATE_TICKET_RESP;
	static std::string GET_TICKET_RESP;
};

std::string MmMessageType::CREATE_TICKET = "create-ticket";
std::string MmMessageType::CREATE_TICKET_RESP = "create-ticket-resp";
std::string MmMessageType::GET_TICKET_RESP = "get-ticket-resp";

Matchmaking::Matchmaking(UserInfo user)
{
	//this->isMex = SConfig::GetInstance().m_slippiCustomMMEnabled && isMex;

	m_user = user;
	m_state = ProcessState::IDLE;
	m_errorMsg = "";

	m_client = nullptr;
	m_server = nullptr;

	MM_HOST = getDefaultMMHost();
	generator = std::default_random_engine(Common::Timer::NowMs());
}

Matchmaking::~Matchmaking()
{
	m_state = ProcessState::ERROR_ENCOUNTERED;
	m_errorMsg = "Matchmaking shut down";

	if (m_matchmakeThread.joinable())
		m_matchmakeThread.join();

	terminateMmConnection();
}

/**
 * Resolves to the Production or development MM Host URI
 * for Vanilla Melee
 * @return
 */
std::string Matchmaking::getDefaultMMHost()
{
	// Use configured override URL if set, otherwise fall back to the built-in prod/dev constant.
	auto configUrl = Config::Get(Config::BRAWLBACK_MM_SERVER_URL);
	if (!configUrl.empty())
		return configUrl;
	auto prodUrl = MM_HOST_PROD;
	auto devUrl = MM_HOST_DEV;
	return scm_slippi_semver_str.find("dev") == std::string::npos ? prodUrl : devUrl;
}

/**
 * Resolves to the Production or development MM Host URI
 * for MEX-type games
 * @return
 */
std::string Matchmaking::getMexMMHost()
{
	auto prodUrl = std::string("");
	auto devUrl = MM_HOST_DEV;
	return scm_slippi_semver_str.find("dev") == std::string::npos ? prodUrl : devUrl;
}

/**
 * Resolves the effective MMHOST to user based on the
 * MatchmakingMode being used
 * For Mex-Type games, only Unranked and Ranked are used
 * All other modes will fallback to Slippi's servers
 * @return
 */
std::string Matchmaking::getMMHostForSearchMode()
{
	//bool isMexMode = Matchmaking::IsMexMode(this->isMex, this->m_searchSettings.mode);
	//return isMexMode ? getMexMMHost() : getDefaultMMHost();
    return getMexMMHost();
}

/**
 * Static Helper to identify if a given MM Mode should go through
 * Slippi Servers or not
 * @param isCurrentGameMex that indicates if current game is Mex Type
 * @param mode
 * @return false if the mode should go through Slippi Servers
 */
/*
bool Matchmaking::IsMexMode(bool isCurrentGameMex, OnlinePlayMode mode)
{
	if (!isCurrentGameMex)
		return false;

	switch (mode)
	{
	case OnlinePlayMode::UNRANKED:
	case OnlinePlayMode::RANKED:
		return true;
	default:
		return false;
	}
}
*/

void Matchmaking::FindMatch(MatchSearchSettings settings)
{
	// We do this here again because an instance could already be created and
	// without this, executing FindMatch could drop custom mm players into
	// slippi servers
	//this->isMex = SConfig::GetInstance().m_slippiCustomMMEnabled && isMex;
	isMmConnected = false;

	ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Starting matchmaking...");

	m_searchSettings = settings;

	m_errorMsg = "";
	m_state = ProcessState::INITIALIZING;
	m_matchmakeThread = std::thread(&Matchmaking::MatchmakeThread, this);
}

Matchmaking::ProcessState Matchmaking::GetMatchmakeState()
{
	return m_state;
}

void Matchmaking::SetMatchmakeState(ProcessState state)
{
  m_state = state;
}

std::string Matchmaking::GetErrorMessage()
{
	return m_errorMsg;
}

bool Matchmaking::IsSearching()
{
	return searchingStates.count(m_state) != 0;
}

bool Matchmaking::IsHost()
{
  return m_isHost;
}

u16 Matchmaking::GetLocalPort()
{
  return m_hostPort;
}

bool Matchmaking::IsFixedRulesMode(OnlinePlayMode mode)
{
	return mode == Matchmaking::OnlinePlayMode::UNRANKED ||
		mode == Matchmaking::OnlinePlayMode::RANKED;
}

void Matchmaking::sendMessage(json msg)
{
	enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
	u8 channelId = 0;

	std::string msgContents = msg.dump();

	ENetPacket *epac = enet_packet_create(msgContents.c_str(), msgContents.length(), flags);
	enet_peer_send(m_server, channelId, epac);
}

int Matchmaking::receiveMessage(json &msg, int timeoutMs)
{
	int hostServiceTimeoutMs = 250;

	// Make sure loop runs at least once
	if (timeoutMs < hostServiceTimeoutMs)
		timeoutMs = hostServiceTimeoutMs;

	// This is not a perfect way to timeout but hopefully it's close enough?
	int maxAttempts = timeoutMs / hostServiceTimeoutMs;

	for (int i = 0; i < maxAttempts; i++)
	{
		ENetEvent netEvent;
		int net = enet_host_service(m_client, &netEvent, hostServiceTimeoutMs);
		if (net <= 0)
			continue;

		switch (netEvent.type)
		{
		case ENET_EVENT_TYPE_RECEIVE:
		{

			std::vector<u8> buf;
			buf.insert(buf.end(), netEvent.packet->data, netEvent.packet->data + netEvent.packet->dataLength);

			std::string str(buf.begin(), buf.end());
			msg = json::parse(str);
			// ERROR_LOG(BRAWLBACK, "[Matchmaking] MESSAGE: %s", msg.dump());

			enet_packet_destroy(netEvent.packet);
			return 0;
		}
		case ENET_EVENT_TYPE_DISCONNECT:
			// Return -2 code to indicate we have lost connection to the server
			return -2;
		}
	}

	return -1;
}

void Matchmaking::MatchmakeThread()
{
    Common::SetCurrentThreadName("BrawlbackMatchmakingPhase1");
	while (IsSearching())
	{
		switch (m_state)
		{
		case ProcessState::INITIALIZING:
			startMatchmaking();
			break;
		case ProcessState::MATCHMAKING:
			handleMatchmaking();
			break;
		case ProcessState::OPPONENT_CONNECTING:
			handleConnecting();
			break;
		}
	}

	// Clean up ENET connections
	terminateMmConnection();
}

void Matchmaking::disconnectFromServer()
{
	isMmConnected = false;

	if (m_server)
		enet_peer_disconnect(m_server, 0);
	else
		return;

	ENetEvent netEvent;
	while (enet_host_service(m_client, &netEvent, 3000) > 0)
	{
		switch (netEvent.type)
		{
		case ENET_EVENT_TYPE_RECEIVE:
			enet_packet_destroy(netEvent.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			m_server = nullptr;
			return;
		default:
			break;
		}
	}

	// didn't disconnect gracefully force disconnect
	enet_peer_reset(m_server);
	m_server = nullptr;
}

void Matchmaking::terminateMmConnection()
{
	// Disconnect from server
	disconnectFromServer();

	// Destroy client
	if (m_client)
	{
		enet_host_destroy(m_client);
		m_client = nullptr;
	}
}

void Matchmaking::startMatchmaking()
{
	// I don't understand why I have to do this... if I don't do this, rand always returns the
	// same value
	m_client = nullptr;

	int retryCount = 0;
	auto userInfo = m_user;
	while (m_client == nullptr && retryCount < 15)
	{
		bool customPort = Config::Get(Config::BRAWLBACK_FORCE_PORT);

		if (customPort) {
			m_hostPort = Config::Get(Config::BRAWLBACK_NETPLAY_PORT);
            INFO_LOG_FMT(BRAWLBACK, "Using custom port: {}\n", m_hostPort);
        }
		else
			m_hostPort = 41000 + (generator() % 10000);
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Port to use: {}...", m_hostPort);

		// We are explicitly setting the client address because we are trying to utilize our connection
		// to the matchmaking service in order to hole punch. This port will end up being the port
		// we listen on when we start our server
		ENetAddress clientAddr;
		clientAddr.host = ENET_HOST_ANY;
		clientAddr.port = m_hostPort;

		m_client = enet_host_create(&clientAddr, 1, 3, 0, 0);
		retryCount++;
	}

	if (m_client == nullptr)
	{
		// Failed to create client
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = "Failed to create mm client";
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Failed to create client...");
		return;
	}

	ENetAddress addr;
	auto effectiveHost = getMMHostForSearchMode();
	ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] HOST: {}", effectiveHost);

	enet_address_set_host(&addr, effectiveHost.c_str());
	addr.port = MM_PORT;

	m_server = enet_host_connect(m_client, &addr, 3, 0);

	if (m_server == nullptr)
	{
		// Failed to connect to server
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = "Failed to start connection to mm server";
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Failed to start connection to mm server...");
		return;
	}

	// Before we can request a ticket, we must wait for connection to be successful
	int connectAttemptCount = 0;
	while (!isMmConnected)
	{
		ENetEvent netEvent;
		int net = enet_host_service(m_client, &netEvent, 500);
		if (net <= 0 || netEvent.type != ENET_EVENT_TYPE_CONNECT)
		{
			// Not yet connected, will retry
			connectAttemptCount++;
			if (connectAttemptCount >= 20)
			{
				ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Failed to connect to mm server...");
				m_state = ProcessState::ERROR_ENCOUNTERED;
				m_errorMsg = "Failed to connect to mm server";
				return;
			}

			continue;
		}

		netEvent.peer->data = &userInfo.displayName;
		m_client->intercept = Common::ENet::InterceptCallback;
		isMmConnected = true;
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Connected to mm server...");
	}

	ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Trying to find match...");

	/*if (!m_user->IsLoggedIn())
	{
	    ERROR_LOG(BRAWLBACK, "[Matchmaking] Must be logged in to queue");
	    m_state = ProcessState::ERROR_ENCOUNTERED;
	    m_errorMsg = "Must be logged in to queue. Go back to menu";
	    return;
	}*/

	// The following code attempts to fetch the LAN IP such that when remote IPs match, the
	// LAN IP can be tried in order to establish a connection in the case where the players
	// don't have NAT loopback which allows that type of connection.
	// Right now though, the logic would replace the WAN IP with the LAN IP and if the LAN
	// IP connection didn't work but WAN would have, the players can no longer connect.
	// Two things need to happen to improtve this logic:
	// 1. The connection must be changed to try both the LAN and WAN IPs in the case of
	//    matching WAN IPs
	// 2. The process for fetching LAN IP must be improved. For me, the current method
	//    would always fetch my VirtualBox IP, which is not correct. I also think perhaps
	//    it didn't work on Linux/Mac but I haven't tested it.
	// I left this logic on for now under the assumption that it will help more people than
	// it will hurt
	char lanAddr[30] = "";

	char host[256];
  char* IP = (char*)"NONE";
	struct hostent *host_entry;
	int hostname;
	hostname = gethostname(host, sizeof(host)); // find the host name
	if (hostname == -1)
	{
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Error finding LAN address");
	}
	else
	{
		host_entry = (hostent*)gethostbyname(host); // find host information
		if (host_entry == NULL || host_entry->h_addrtype != AF_INET)
		{
			ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Error finding LAN host");
		}
		else
		{
			// Fetch the last IP (because that was correct for me, not sure if it will be for all)
			int i = 0;
			while (host_entry->h_addr_list[i] != 0)
			{
				IP = inet_ntoa(*((struct in_addr *)host_entry->h_addr_list[i]));
				WARN_LOG_FMT(BRAWLBACK, "[Matchmaking] IP at idx {}: {}", i, IP);
				i++;
			}

			sprintf(lanAddr, "%s:%d", IP, m_hostPort);
		}
	}

	if (Config::Get(Config::BRAWLBACK_FORCE_LAN_IP))
	{
		WARN_LOG_FMT(BRAWLBACK, "[Matchmaking] Overwriting LAN IP sent with configured address");
		sprintf(lanAddr, "%s:%d", Config::Get(Config::BRAWLBACK_LAN_IP).c_str(), m_hostPort);
	}

	WARN_LOG_FMT(BRAWLBACK, "[Matchmaking] Sending LAN address: {}", lanAddr);

	std::vector<u8> connectCodeBuf;
	connectCodeBuf.insert(connectCodeBuf.end(), m_searchSettings.connectCode.begin(),
	                      m_searchSettings.connectCode.end());

	// Send message to server to create ticket
	json request;
	request["type"] = MmMessageType::CREATE_TICKET;
	request["user"] = {{"uid", userInfo.uid}, {"playKey", userInfo.playKey}};
	request["search"] = {
	        {"mode", m_searchSettings.mode},
	        {"connectCode", connectCodeBuf},
	        {"game", {
                //{"id", SConfig::GetInstance().GetGameID()},
                {"id", "RSBE01"},
                //{"ex_id", SConfig::GetInstance().GetGameID()},
                {"ex_id", "RSBE01"},
	              {"revision", 0},
                {"type", 1},// GameType::VANILLA
                {"name", std::string("")},
	        }}
	};
	request["appVersion"] = scm_slippi_semver_str;
	request["ipAddressLan"] = lanAddr;
	sendMessage(request);

	// Get response from server
	json response;
	int rcvRes = receiveMessage(response, 5000);
	if (rcvRes != 0)
	{
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Did not receive response from server for create ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = "Failed to join mm queue";
		return;
	}

	std::string respType = response["type"];
	if (respType != MmMessageType::CREATE_TICKET_RESP)
	{
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Received incorrect response for create ticket");
		ERROR_LOG_FMT(BRAWLBACK, "{}", response.dump());
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = "Invalid response when joining mm queue";
		return;
	}

	std::string err = response.value("error", "");
	if (err.length() > 0)
	{
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Received error from server for create ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = err;
		return;
	}

	m_state = ProcessState::MATCHMAKING;
	ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Request ticket success");
}

void Matchmaking::handleConnecting()
{
  // Don't do anything, for now it's being handled on a thread on EXI
}

void Matchmaking::handleMatchmaking()
{
	// Deal with class shut down
	if (m_state != ProcessState::MATCHMAKING)
		return;

	// Get response from server
	json getResp;
	int rcvRes = receiveMessage(getResp, 2000);
	if (rcvRes == -1)
	{
		INFO_LOG_FMT(BRAWLBACK, "[Matchmaking] Have not yet received assignment");
		return;
	}
	else if (rcvRes != 0)
	{
		// Right now the only other code is -2 meaning the server died probably?
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Lost connection to the mm server");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = "Lost connection to the mm server";
		return;
	}

	std::string respType = getResp["type"];
	if (respType != MmMessageType::GET_TICKET_RESP)
	{
		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Received incorrect response for get ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = "Invalid response when getting mm status";
		return;
	}

	std::string err = getResp.value("error", "");
	std::string latestVersion = getResp.value("latestVersion", "");
	if (err.length() > 0)
	{
		if (latestVersion != "")
		{
			// Update version number when the mm server tells us our version is outdated
			//m_user->OverwriteLatestVersion(
			//    latestVersion); // Force latest version for people whose file updates dont work
		}

		ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Received error from server for get ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		m_errorMsg = err;
		return;
	}

	m_isSwapAttempt = false;
	//m_netplayClient = nullptr;

	// Clear old users
	m_remoteIps.clear();
	m_playerInfo.clear();

	auto queue = getResp["players"];
	if (queue.is_array())
	{
		std::string localExternalIp = "";

		for (json::iterator it = queue.begin(); it != queue.end(); ++it)
		{
			json el = *it;
			UserInfo playerInfo;

			bool isLocal = el.value("isLocalPlayer", false);
			playerInfo.uid = el.value("uid", "");
			playerInfo.displayName = el.value("displayName", "");
			playerInfo.connectCode = el.value("connectCode", "");
			playerInfo.port = el.value("port", 0);
			m_playerInfo.push_back(playerInfo);

			if (isLocal)
			{
				std::vector<std::string> localIpParts = SplitString(el.value("ipAddress", "1.1.1.1:123"), ':');

				localExternalIp = localIpParts[0];
				m_localPlayerIndex = playerInfo.port - 1;
			}
		};

		// Loop a second time to get the correct remote IPs
		for (json::iterator it = queue.begin(); it != queue.end(); ++it)
		{
			json el = *it;

			if (el.value("port", 0) - 1 == m_localPlayerIndex)
				continue;

			auto extIp = el.value("ipAddress", "1.1.1.1:123");
			std::vector<std::string> exIpParts = SplitString(extIp, ':');

			auto lanIp = el.value("ipAddressLan", "1.1.1.1:123");

			WARN_LOG_FMT(BRAWLBACK, "LAN IP: {}", lanIp);

			if (exIpParts[0] != localExternalIp || lanIp.empty())
			{
				// If external IPs are different, just use that address
				m_remoteIps.push_back(extIp);
				continue;
			}

			// TODO: Instead of using one or the other, it might be better to try both

			// If external IPs are the same, try using LAN IPs
			m_remoteIps.push_back(lanIp);
		}
	}
	m_isHost = getResp.value("isHost", false);

	// Get allowed stages. For stage select modes like direct and teams, this will only impact the first map selected
	m_allowedStages.clear();
	auto stages = getResp["stages"];
	if (stages.is_array())
	{
		for (json::iterator it = stages.begin(); it != stages.end(); ++it)
		{
			json el = *it;
			auto stageId = el.get<int>();
			m_allowedStages.push_back(stageId);
		}
	}

	// Disconnect and destroy enet client to mm server
	terminateMmConnection();

	m_state = ProcessState::OPPONENT_CONNECTING;
	ERROR_LOG_FMT(BRAWLBACK, "[Matchmaking] Opponent found. isDecider: {}", m_isHost ? "true" : "false");
}

int Matchmaking::LocalPlayerIndex()
{
	return m_localPlayerIndex;
}

std::vector<UserInfo> Matchmaking::GetPlayerInfo()
{
	return m_playerInfo;
}

std::vector<u16> Matchmaking::GetStages()
{
	return m_allowedStages;
}

u16 Matchmaking::GetRandomStage() {
  if (m_allowedStages.empty())
  {
    // Default case, shouldn't ever really be hit but it's here just in case
    m_allowedStages.push_back(0x1);  // Battlefield
    m_allowedStages.push_back(0x2);  // FD
    // m_allowedStages.push_back(0x3); // Delfino's Secret
    m_allowedStages.push_back(0x5);  // Metal Cavern
    // m_allowedStages.push_back(0x0D); // Yoshi's Island
    // m_allowedStages.push_back(0x2A); // Yoshi's Story
    m_allowedStages.push_back(0x1C);  // Wario land
    // m_allowedStages.push_back(0x2D); // Dream land
    m_allowedStages.push_back(0x2E);  // PS2
                                      // m_allowedStages.push_back(0x23); // Green hill zone
    // m_allowedStages.push_back(0x21); // Smashville

    // Add FoD if singles
    if (m_playerInfo.size() == 2)
    {
      // m_allowedStages.push_back(0x1F); // FoD
    }
  }

  int randStageIdx = generator() % m_allowedStages.size();
  for (u16 stageID : m_allowedStages) {
      INFO_LOG_FMT(BRAWLBACK, "Allowed stage id: {}\n", (unsigned int)stageID);
  }
  INFO_LOG_FMT(BRAWLBACK, "rand stage idx chosen: {}\n", randStageIdx);
  INFO_LOG_FMT(BRAWLBACK, "rand stage id chosen: {}\n", (unsigned int)m_allowedStages[randStageIdx]);
  return m_allowedStages[randStageIdx];
}

std::string Matchmaking::GetPlayerName(u8 port)
{
	if (port >= m_playerInfo.size())
	{
		return "";
	}
	return m_playerInfo[port].displayName;
}

u8 Matchmaking::RemotePlayerCount()
{
	if (m_playerInfo.size() == 0)
		return 0;

	return (u8)m_playerInfo.size() - 1;
}

std::vector<std::string> Matchmaking::GetRemoteIPAddresses() {
    std::vector<std::string> addrs;
    for (int i = 0; i < m_remoteIps.size(); i++)
    {
        addrs.push_back(SplitString(m_remoteIps[i], ':')[0]);
    }
    return addrs;
}

std::vector<u16> Matchmaking::GetRemotePorts() {
    std::vector<u16> ports;
    for (int i = 0; i < m_remoteIps.size(); i++)
    {
        ports.push_back(std::stoi(SplitString(m_remoteIps[i], ':')[1]));
    }
    return ports;
}
