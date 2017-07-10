/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <new>
#include <engine/shared/config.h>
#include "player.h"

#include <engine/server.h>
#include <engine/server/server.h>
#include "gamecontext.h"
#include <game/gamecore.h>
#include <game/version.h>
#include <game/server/teams.h>
#include "gamemodes/DDRace.h"
#include <stdio.h>
#include <time.h>

#include "specialchars.h"

inline int ms_rand(int *seed)
{
	*seed = *seed * 0x343fd + 0x269EC3;  // a=214013, b=2531011
	return (*seed >> 0x10) & 0x7FFF;
}

// Character, "physical" player's part
#define FeatureCapture(X) m_ ## X
MACRO_ALLOC_POOL_ID_IMPL(CPlayer, MAX_CLIENTS)

IServer *CPlayer::Server() const { return m_pGameServer->Server(); }

CPlayer::CPlayer(CGameContext *pGameServer, int ClientID, int Team)
{
	FeatureCapture(pGameServer) = pGameServer;
	FeatureCapture(ClientID) = ClientID;
	FeatureCapture(Team) = GameServer()->m_pController->ClampTeam(Team);
	FeatureCapture(pCharacter) = 0;
	FeatureCapture(NumInputs) = 0;
	FeatureCapture(KillMe) = 0;
	FeatureCapture(EpicCircle) = false;
	FeatureCapture(IsBallSpawned) = false;
	FeatureCapture(RainbowHook) = false;
	FeatureCapture(Lovely) = false;
	FeatureCapture(HeartGuns) = false;
	FeatureCapture(Stars) = false;
	FeatureCapture(Invisible) = false;
	FeatureCapture(Rainbowepiletic) = false;
	FeatureCapture(Rainbow) = false;
	FeatureCapture(Blackhole) = false;
	FeatureCapture(ShowWhispers) = false;

	Reset();
}

CPlayer::~CPlayer()
{
	delete m_pCharacter;
	m_pCharacter = 0;
}

void CPlayer::Reset()
{
	m_RandIndex = rand() % 16;
	m_pSkin = aSkins[m_RandIndex];
	m_DieTick = Server()->Tick();
	m_JoinTick = Server()->Tick();
	if (m_pCharacter)
		delete m_pCharacter;
	m_pCharacter = 0;
	m_KillMe = 0;
	m_SpectatorID = SPEC_FREEVIEW;
	m_LastActionTick = Server()->Tick();
	m_TeamChangeTick = Server()->Tick();
	m_WeakHookSpawn = false;
	m_SilentMuted = false;

	// city - label everything vali so its easier to find pls
	m_pAccount = new CAccount(this);
	//m_pAccount->SetStorage(GameServer()->Storage());
	if (m_AccData.m_UserID)
		m_pAccount->Apply();

	int* idMap = Server()->GetIdMap(m_ClientID);
	for (int i = 1; i < VANILLA_MAX_CLIENTS; i++)
	{
		idMap[i] = -1;
	}
	idMap[0] = m_ClientID;

	// DDRace

	m_LastCommandPos = 0;
	m_LastPlaytime = time_get();
	m_Sent1stAfkWarning = 0;
	m_Sent2ndAfkWarning = 0;
	m_ChatScore = 0;
	m_EyeEmote = true;
	m_TimerType = g_Config.m_SvDefaultTimerType;
	m_DefEmote = EMOTE_NORMAL;
	m_Afk = false;
	m_LastWhisperTo = -1;
	m_LastSetSpectatorMode = 0;
	m_TimeoutCode[0] = '\0';

	m_TuneZone = 0;
	m_TuneZoneOld = m_TuneZone;
	m_Halloween = false;
	m_FirstPacket = true;

	m_SendVoteIndex = -1;

	if (g_Config.m_SvEvents)
	{
		time_t rawtime;
		struct tm* timeinfo;
		char d[16], m[16], y[16];
		int dd, mm;
		time(&rawtime);
		timeinfo = localtime(&rawtime);
		strftime(d, sizeof(y), "%d", timeinfo);
		strftime(m, sizeof(m), "%m", timeinfo);
		strftime(y, sizeof(y), "%Y", timeinfo);
		dd = atoi(d);
		mm = atoi(m);
		if ((mm == 12 && dd == 31) || (mm == 1 && dd == 1))
		{ // New Year
			m_DefEmote = EMOTE_HAPPY;
		}
		else if ((mm == 10 && dd == 31) || (mm == 11 && dd == 1))
		{ // Halloween
			m_DefEmote = EMOTE_ANGRY;
			m_Halloween = true;
		}
		else
		{
			m_DefEmote = EMOTE_NORMAL;
		}
	}
	m_DefEmoteReset = -1;

	GameServer()->Score()->PlayerData(m_ClientID)->Reset();

	m_ClientVersion = VERSION_VANILLA;
	m_ShowOthers = g_Config.m_SvShowOthersDefault;
	m_ShowAll = g_Config.m_SvShowAllDefault;
	m_SpecTeam = 0;
	m_NinjaJetpack = false;

	m_Paused = PAUSED_NONE;
	m_DND = false;

	m_NextPauseTick = 0;

	// Variable initialized:
	m_Last_Team = 0;
#if defined(CONF_SQL)
	m_LastSQLQuery = 0;
#endif

	int64 Now = Server()->Tick();
	int64 TickSpeed = Server()->TickSpeed();
	// If the player joins within ten seconds of the server becoming
	// non-empty, allow them to vote immediately. This allows players to
	// vote after map changes or when they join an empty server.
	//
	// Otherwise, block voting for 60 seconds after joining.
	if (Now > GameServer()->m_NonEmptySince + 10 * TickSpeed)
		m_FirstVoteTick = Now + g_Config.m_SvJoinVoteDelay * TickSpeed;
	else
	{
		m_FirstVoteTick = Now;
	}

	m_InLMB = LMB_NONREGISTERED;

	m_SavedStats.Reset();
}

void CPlayer::Tick()
{
#ifdef CONF_DEBUG
	if (!g_Config.m_DbgDummies || m_ClientID < MAX_CLIENTS - g_Config.m_DbgDummies)
#endif
		if (!Server()->ClientIngame(m_ClientID))
			return;	

	if (m_KillMe != 0)
	{
		KillCharacter(m_KillMe);
		m_KillMe = 0;
		return;
	}

	if (m_ChatScore > 0)
		m_ChatScore--;

	if (m_ForcePauseTime > 0)
		m_ForcePauseTime--;

	Server()->SetClientScore(m_ClientID, m_Score);

	// do latency stuff
	{
		IServer::CClientInfo Info;
		if (Server()->GetClientInfo(m_ClientID, &Info))
		{
			m_Latency.m_Accum += Info.m_Latency;
			m_Latency.m_AccumMax = max(m_Latency.m_AccumMax, Info.m_Latency);
			m_Latency.m_AccumMin = min(m_Latency.m_AccumMin, Info.m_Latency);
		}
		// each second
		if (Server()->Tick() % Server()->TickSpeed() == 0)
		{
			m_Latency.m_Avg = m_Latency.m_Accum / Server()->TickSpeed();
			m_Latency.m_Max = m_Latency.m_AccumMax;
			m_Latency.m_Min = m_Latency.m_AccumMin;
			m_Latency.m_Accum = 0;
			m_Latency.m_AccumMin = 1000;
			m_Latency.m_AccumMax = 0;
		}
	}

	if (((CServer *)Server())->m_NetServer.ErrorString(m_ClientID)[0])
	{
		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "'%s' would have timed out, but can use timeout protection now", Server()->ClientName(m_ClientID));
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
		((CServer *)(Server()))->m_NetServer.ResetErrorString(m_ClientID);
	}

	if (!GameServer()->m_World.m_Paused)
	{
		if (!m_pCharacter && (m_DieTick + Server()->TickSpeed() * 3 <= Server()->Tick() || m_InLMB == LMB_PARTICIPATE))
			m_Spawning = true;

		if (m_pCharacter)
		{
			if (m_pCharacter->IsAlive())
			{
				if (m_Paused >= PAUSED_FORCE)
				{
					if (m_ForcePauseTime == 0)
						m_Paused = PAUSED_NONE;
					ProcessPause();
				}
				else if (m_Paused == PAUSED_PAUSED && m_NextPauseTick < Server()->Tick())
				{
					if ((!m_pCharacter->GetWeaponGot(WEAPON_NINJA) || m_pCharacter->m_FreezeTime) && m_pCharacter->IsGrounded() && m_pCharacter->m_Pos == m_pCharacter->m_PrevPos)
						ProcessPause();
				}
				else if (m_NextPauseTick < Server()->Tick())
				{
					ProcessPause();
				}
				if (!m_Paused)
					m_ViewPos = m_pCharacter->m_Pos;
			}
			else if (!m_pCharacter->IsPaused())
			{
				delete m_pCharacter;
				m_pCharacter = 0;
			}
		}
		else if (m_Spawning && !m_WeakHookSpawn)
			TryRespawn();
	}
	else
	{
		++m_DieTick;
		++m_JoinTick;
		++m_LastActionTick;
		++m_TeamChangeTick;
	}

	m_TuneZoneOld = m_TuneZone; // determine needed tunings with viewpos
	int CurrentIndex = GameServer()->Collision()->GetMapIndex(m_ViewPos);
	m_TuneZone = GameServer()->Collision()->IsTune(CurrentIndex);

	if (m_TuneZone != m_TuneZoneOld) // dont send tunigs all the time
	{
		GameServer()->SendTuningParams(m_ClientID, m_TuneZone);
	}

	HandleQuest();
}

void CPlayer::PostTick()
{
	// update latency value
	if (m_PlayerFlags&PLAYERFLAG_SCOREBOARD)
	{
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			if (GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
				m_aActLatency[i] = GameServer()->m_apPlayers[i]->m_Latency.m_Min;
		}
	}

	// update view pos for spectators
	if ((m_Team == TEAM_SPECTATORS || m_Paused) && m_SpectatorID != SPEC_FREEVIEW && GameServer()->m_apPlayers[m_SpectatorID] && GameServer()->m_apPlayers[m_SpectatorID]->GetCharacter())
		m_ViewPos = GameServer()->m_apPlayers[m_SpectatorID]->GetCharacter()->m_Pos;
}

void CPlayer::PostPostTick()
{
#ifdef CONF_DEBUG
	if (!g_Config.m_DbgDummies || m_ClientID < MAX_CLIENTS - g_Config.m_DbgDummies)
#endif
		if (!Server()->ClientIngame(m_ClientID))
			return;

	if (!GameServer()->m_World.m_Paused && !m_pCharacter && m_Spawning && m_WeakHookSpawn)
		TryRespawn();
}

void CPlayer::Snap(int SnappingClient)
{
#ifdef CONF_DEBUG
	if (!g_Config.m_DbgDummies || m_ClientID < MAX_CLIENTS - g_Config.m_DbgDummies)
#endif
		
	if (!Server()->ClientIngame(m_ClientID))
		return;

	int id = m_ClientID;
	if (SnappingClient > -1 && !Server()->Translate(id, SnappingClient))
		return;

	if(m_Invisible && SnappingClient != id && !Server()->IsAdmin(SnappingClient))
		return;

	CNetObj_ClientInfo *pClientInfo = static_cast<CNetObj_ClientInfo *>(Server()->SnapNewItem(NETOBJTYPE_CLIENTINFO, id, sizeof(CNetObj_ClientInfo)));

	if (!pClientInfo)
		return;

	if (g_Config.m_SvAnonymousBlock || m_InLMB == LMB_PARTICIPATE)
	{
		StrToInts(&pClientInfo->m_Name0, 4, " ");
		StrToInts(&pClientInfo->m_Clan0, 3, " ");
		pClientInfo->m_Country = -1;
	}
	else
	{
		StrToInts(&pClientInfo->m_Name0, 4, Server()->ClientName(m_ClientID));
		StrToInts(&pClientInfo->m_Clan0, 3, Server()->ClientClan(m_ClientID));
		pClientInfo->m_Country = Server()->ClientCountry(m_ClientID);
	}

	if (m_StolenSkin && SnappingClient != m_ClientID && g_Config.m_SvSkinStealAction == 1)
	{
		StrToInts(&pClientInfo->m_Skin0, 6, "pinky");
		pClientInfo->m_UseCustomColor = 0;
		pClientInfo->m_ColorBody = m_TeeInfos.m_ColorBody;
		pClientInfo->m_ColorFeet = m_TeeInfos.m_ColorFeet;
	}
	else
	{
		if (g_Config.m_SvAnonymousBlock || m_InLMB == LMB_PARTICIPATE)
		{
			if (Server()->Tick() >= m_LastTriggerTick + Server()->TickSpeed() * 2) {
				m_LastTriggerTick = Server()->Tick();
				m_RandIndex = rand() % 16;
				m_pSkin = aSkins[m_RandIndex];
			}
			StrToInts(&pClientInfo->m_Skin0, 6, m_pSkin);
			pClientInfo->m_ColorBody = m_TeeInfos.m_ColorBody;
			pClientInfo->m_ColorFeet = m_TeeInfos.m_ColorFeet;
			pClientInfo->m_UseCustomColor = 0;
		}
		else
		{
			StrToInts(&pClientInfo->m_Skin0, 6, m_TeeInfos.m_SkinName);
			pClientInfo->m_UseCustomColor = m_TeeInfos.m_UseCustomColor;
			pClientInfo->m_ColorBody = m_TeeInfos.m_ColorBody;
			pClientInfo->m_ColorFeet = m_TeeInfos.m_ColorFeet;
		}
	}

	CNetObj_PlayerInfo *pPlayerInfo = static_cast<CNetObj_PlayerInfo *>(Server()->SnapNewItem(NETOBJTYPE_PLAYERINFO, id, sizeof(CNetObj_PlayerInfo)));
	if (!pPlayerInfo)
		return;

	pPlayerInfo->m_Latency = SnappingClient == -1 ? m_Latency.m_Min : GameServer()->m_apPlayers[SnappingClient]->m_aActLatency[m_ClientID];
	pPlayerInfo->m_Local = 0;
	pPlayerInfo->m_ClientID = id;
	pPlayerInfo->m_Score = abs(m_Score) * -1;
	pPlayerInfo->m_Team = (m_ClientVersion < VERSION_DDNET_OLD || m_Paused != PAUSED_SPEC || m_ClientID != SnappingClient) && m_Paused < PAUSED_PAUSED ? m_Team : TEAM_SPECTATORS;

	if (m_ClientID == SnappingClient && (m_Paused != PAUSED_SPEC || m_ClientVersion >= VERSION_DDNET_OLD))
		pPlayerInfo->m_Local = 1;

	if (m_ClientID == SnappingClient && (m_Team == TEAM_SPECTATORS || m_Paused))
	{
		CNetObj_SpectatorInfo *pSpectatorInfo = static_cast<CNetObj_SpectatorInfo *>(Server()->SnapNewItem(NETOBJTYPE_SPECTATORINFO, m_ClientID, sizeof(CNetObj_SpectatorInfo)));
		if (!pSpectatorInfo)
			return;

		pSpectatorInfo->m_SpectatorID = m_SpectatorID;
		pSpectatorInfo->m_X = m_ViewPos.x;
		pSpectatorInfo->m_Y = m_ViewPos.y;
	}

	// send 0 if times of others are not shown
	if (SnappingClient != m_ClientID && g_Config.m_SvHideScore)
		pPlayerInfo->m_Score = -9999;
	else
		pPlayerInfo->m_Score = abs(m_Score) * -1;

	if (g_Config.m_SvAnonymousBlock || m_InLMB == LMB_PARTICIPATE)
		pPlayerInfo->m_Score = -9999;
}

void CPlayer::FakeSnap()
{
	// This is problematic when it's sent before we know whether it's a non-64-player-client
	// Then we can't spectate players at the start

	if (m_ClientVersion >= VERSION_DDNET_OLD)
		return;

	int FakeID = VANILLA_MAX_CLIENTS - 1;

	CNetObj_ClientInfo *pClientInfo = static_cast<CNetObj_ClientInfo *>(Server()->SnapNewItem(NETOBJTYPE_CLIENTINFO, FakeID, sizeof(CNetObj_ClientInfo)));

	if (!pClientInfo)
		return;

	StrToInts(&pClientInfo->m_Name0, 4, " ");
	StrToInts(&pClientInfo->m_Clan0, 3, " ");
	StrToInts(&pClientInfo->m_Skin0, 6, m_pSkin);

	if (m_Paused != PAUSED_SPEC)
		return;

	CNetObj_PlayerInfo *pPlayerInfo = static_cast<CNetObj_PlayerInfo *>(Server()->SnapNewItem(NETOBJTYPE_PLAYERINFO, FakeID, sizeof(CNetObj_PlayerInfo)));
	if (!pPlayerInfo)
		return;

	pPlayerInfo->m_Latency = m_Latency.m_Min;
	pPlayerInfo->m_Local = 1;
	pPlayerInfo->m_ClientID = FakeID;
	pPlayerInfo->m_Score = -9999;
	pPlayerInfo->m_Team = TEAM_SPECTATORS;

	CNetObj_SpectatorInfo *pSpectatorInfo = static_cast<CNetObj_SpectatorInfo *>(Server()->SnapNewItem(NETOBJTYPE_SPECTATORINFO, FakeID, sizeof(CNetObj_SpectatorInfo)));
	if (!pSpectatorInfo)
		return;

	pSpectatorInfo->m_SpectatorID = m_SpectatorID;
	pSpectatorInfo->m_X = m_ViewPos.x;
	pSpectatorInfo->m_Y = m_ViewPos.y;
}

void CPlayer::OnDisconnect(const char *pReason)
{
	if (m_AccData.m_UserID)
	{
		//m_pAccount->SetStorage(GameServer()->Storage());
		m_AccData.m_Slot--;
		m_pAccount->Apply(); // Save important Shit b4 leaving
		m_pAccount->Reset();
	}

	KillCharacter();

	if (Server()->ClientIngame(m_ClientID))
	{
		char aBuf[512];
		if (pReason && *pReason)
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game (%s)", Server()->ClientName(m_ClientID), pReason);
		else
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game", Server()->ClientName(m_ClientID));
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);

		str_format(aBuf, sizeof(aBuf), "leave player='%d:%s'", m_ClientID, Server()->ClientName(m_ClientID));
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "game", aBuf);
	}

	CGameControllerDDRace* Controller = (CGameControllerDDRace*)GameServer()->m_pController;
	Controller->m_Teams.SetForceCharacterTeam(m_ClientID, 0);
}

void CPlayer::OnPredictedInput(CNetObj_PlayerInput *NewInput)
{
	// skip the input if chat is active
	if ((m_PlayerFlags&PLAYERFLAG_CHATTING) && (NewInput->m_PlayerFlags&PLAYERFLAG_CHATTING))
		return;

	AfkVoteTimer(NewInput);

	m_NumInputs++;

	if (m_pCharacter && !m_Paused)
		m_pCharacter->OnPredictedInput(NewInput);

	// Magic number when we can hope that client has successfully identified itself
	if (m_NumInputs == 20)
	{
		if (g_Config.m_SvClientSuggestion[0] != '\0' && m_ClientVersion <= VERSION_DDNET_OLD)
			GameServer()->SendBroadcast(g_Config.m_SvClientSuggestion, m_ClientID);
	}
}

void CPlayer::OnDirectInput(CNetObj_PlayerInput *NewInput)
{
	if (AfkTimer(NewInput->m_TargetX, NewInput->m_TargetY))
		return; // we must return if kicked, as player struct is already deleted
	AfkVoteTimer(NewInput);

	if (NewInput->m_PlayerFlags&PLAYERFLAG_CHATTING)
	{
		// skip the input if chat is active
		if (m_PlayerFlags&PLAYERFLAG_CHATTING)
			return;

		// reset input
		if (m_pCharacter)
			m_pCharacter->ResetInput();

		if (!g_Config.m_SvAnonymousBlock)
			m_PlayerFlags = NewInput->m_PlayerFlags;
		return;
	}

	m_PlayerFlags = NewInput->m_PlayerFlags;

	if (m_pCharacter)
	{
		if (!m_Paused)
			m_pCharacter->OnDirectInput(NewInput);
		else
			m_pCharacter->ResetInput();
	}

	if (!m_pCharacter && m_Team != TEAM_SPECTATORS && (NewInput->m_Fire & 1))
		m_Spawning = true;

	if (((!m_pCharacter && m_Team == TEAM_SPECTATORS) || m_Paused) && m_SpectatorID == SPEC_FREEVIEW)
		m_ViewPos = vec2(NewInput->m_TargetX, NewInput->m_TargetY);

	// check for activity
	if (NewInput->m_Direction || m_LatestActivity.m_TargetX != NewInput->m_TargetX ||
		m_LatestActivity.m_TargetY != NewInput->m_TargetY || NewInput->m_Jump ||
		NewInput->m_Fire & 1 || NewInput->m_Hook)
	{
		m_LatestActivity.m_TargetX = NewInput->m_TargetX;
		m_LatestActivity.m_TargetY = NewInput->m_TargetY;
		m_LastActionTick = Server()->Tick();
	}
}

CCharacter *CPlayer::GetCharacter()
{
	if (m_pCharacter && m_pCharacter->IsAlive())
		return m_pCharacter;
	return 0;
}

void CPlayer::ThreadKillCharacter(int Weapon)
{
	m_KillMe = Weapon;
}

void CPlayer::KillCharacter(int Weapon)
{
	if (m_pCharacter)
	{
		m_pCharacter->Die(m_ClientID, Weapon);

		delete m_pCharacter;
		m_pCharacter = 0;
	}
}

void CPlayer::Respawn(bool WeakHook)
{
	if (m_Team != TEAM_SPECTATORS)
	{
		m_WeakHookSpawn = WeakHook;
		m_Spawning = true;
	}
}

CCharacter* CPlayer::ForceSpawn(vec2 Pos)
{
	m_Spawning = false;
	m_pCharacter = new(m_ClientID) CCharacter(&GameServer()->m_World);
	m_pCharacter->Spawn(this, Pos);
	m_Team = 0;
	return m_pCharacter;
}

void CPlayer::SetTeam(int Team, bool DoChatMsg)
{
	// clamp the team
	Team = GameServer()->m_pController->ClampTeam(Team);
	if (m_Team == Team)
		return;

	char aBuf[512];
	if (DoChatMsg)
	{
		str_format(aBuf, sizeof(aBuf), "'%s' joined the %s", Server()->ClientName(m_ClientID), GameServer()->m_pController->GetTeamName(Team));
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
	}

	if (Team == TEAM_SPECTATORS)
	{
		CGameControllerDDRace* Controller = (CGameControllerDDRace*)GameServer()->m_pController;
		Controller->m_Teams.SetForceCharacterTeam(m_ClientID, 0);
	}

	KillCharacter();

	m_Team = Team;
	m_LastSetTeam = Server()->Tick();
	m_LastActionTick = Server()->Tick();
	m_SpectatorID = SPEC_FREEVIEW;
	str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' m_Team=%d", m_ClientID, Server()->ClientName(m_ClientID), m_Team);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	//GameServer()->m_pController->OnPlayerInfoChange(GameServer()->m_apPlayers[m_ClientID]);

	if (Team == TEAM_SPECTATORS)
	{
		// update spectator modes
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			if (GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->m_SpectatorID == m_ClientID)
				GameServer()->m_apPlayers[i]->m_SpectatorID = SPEC_FREEVIEW;
		}
	}
}

void CPlayer::TryRespawn()
{
	vec2 SpawnPos;

	int Team = m_Team;

	if (m_InLMB == LMB_PARTICIPATE)	//LMB=1 means registered
		Team += 2;
	if (!GameServer()->m_pController->CanSpawn(Team, &SpawnPos))	//we cant spawn being in LMB!
		return;

	CGameControllerDDRace* Controller = (CGameControllerDDRace*)GameServer()->m_pController;

	m_WeakHookSpawn = false;
	m_Spawning = false;
	m_pCharacter = new(m_ClientID) CCharacter(&GameServer()->m_World);

	if (!m_InLMB && (m_SavedStats.m_SavedSpawn.x || m_SavedStats.m_SavedSpawn.y))
		SpawnPos = m_SavedStats.m_SavedSpawn;

	m_pCharacter->Spawn(this, SpawnPos);
	GameServer()->CreatePlayerSpawn(SpawnPos, m_pCharacter->Teams()->TeamMask(m_pCharacter->Team(), -1, m_ClientID));

	if(!m_InLMB)
		LMBRestore();

	if (g_Config.m_SvTeam == 3)
	{
		int NewTeam = 0;
		for (; NewTeam < TEAM_SUPER; NewTeam++)
			if (Controller->m_Teams.Count(NewTeam) == 0)
				break;

		if (NewTeam == TEAM_SUPER)
			NewTeam = 0;

		Controller->m_Teams.SetForceCharacterTeam(GetCID(), NewTeam);
	}

	if (m_InLMB == LMB_PARTICIPATE)
		m_pCharacter->Freeze(g_Config.m_SvLMBSpawnFreezeTime);
}

bool CPlayer::AfkTimer(int NewTargetX, int NewTargetY)
{
	/*
	afk timer (x, y = mouse coordinates)
	Since a player has to move the mouse to play, this is a better method than checking
	the player's position in the game world, because it can easily be bypassed by just locking a key.
	Frozen players could be kicked as well, because they can't move.
	It also works for spectators.
	returns true if kicked
	*/

	if (m_Authed)
		return false; // don't kick admins
	if (g_Config.m_SvMaxAfkTime == 0)
		return false; // 0 = disabled

	if (NewTargetX != m_LastTarget_x || NewTargetY != m_LastTarget_y)
	{
		m_LastPlaytime = time_get();
		m_LastTarget_x = NewTargetX;
		m_LastTarget_y = NewTargetY;
		m_Sent1stAfkWarning = 0; // afk timer's 1st warning after 50% of sv_max_afk_time
		m_Sent2ndAfkWarning = 0;

	}
	else
	{
		if (!m_Paused)
		{
			// not playing, check how long
			if (m_Sent1stAfkWarning == 0 && m_LastPlaytime < time_get() - time_freq()*(int)(g_Config.m_SvMaxAfkTime*0.5))
			{
				sprintf(
					m_pAfkMsg,
					"You have been afk for %d seconds now. Please note that you get kicked after not playing for %d seconds.",
					(int)(g_Config.m_SvMaxAfkTime*0.5),
					g_Config.m_SvMaxAfkTime
					);
				m_pGameServer->SendChatTarget(m_ClientID, m_pAfkMsg);
				m_Sent1stAfkWarning = 1;
			}
			else if (m_Sent2ndAfkWarning == 0 && m_LastPlaytime < time_get() - time_freq()*(int)(g_Config.m_SvMaxAfkTime*0.9))
			{
				sprintf(
					m_pAfkMsg,
					"You have been afk for %d seconds now. Please note that you get kicked after not playing for %d seconds.",
					(int)(g_Config.m_SvMaxAfkTime*0.9),
					g_Config.m_SvMaxAfkTime
					);
				m_pGameServer->SendChatTarget(m_ClientID, m_pAfkMsg);
				m_Sent2ndAfkWarning = 1;
			}
			else if (m_LastPlaytime < time_get() - time_freq()*g_Config.m_SvMaxAfkTime)
			{
				CServer* serv = (CServer*)m_pGameServer->Server();
				serv->Kick(m_ClientID, "Away from keyboard");
				return true;
			}
		}
	}
	return false;
}

void CPlayer::AfkVoteTimer(CNetObj_PlayerInput *NewTarget)
{
	if (g_Config.m_SvMaxAfkVoteTime == 0)
		return;

	if (mem_comp(NewTarget, &m_LastTarget, sizeof(CNetObj_PlayerInput)) != 0)
	{
		m_LastPlaytime = time_get();
		mem_copy(&m_LastTarget, NewTarget, sizeof(CNetObj_PlayerInput));
	}
	else if (m_LastPlaytime < time_get() - time_freq()*g_Config.m_SvMaxAfkVoteTime)
	{
		m_Afk = true;
		return;
	}

	m_Afk = false;
}

void CPlayer::ProcessPause()
{
	if (!m_pCharacter)
		return;

	char aBuf[128];
	if (m_Paused >= PAUSED_PAUSED)
	{
		if (!m_pCharacter->IsPaused())
		{
			m_pCharacter->Pause(true);
			if (g_Config.m_SvPauseMessages)
			{
				str_format(aBuf, sizeof(aBuf), (m_Paused == PAUSED_PAUSED) ? "'%s' paused" : "'%s' was force-paused for %ds", Server()->ClientName(m_ClientID), m_ForcePauseTime / Server()->TickSpeed());
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			}
			GameServer()->CreateDeath(m_pCharacter->m_Pos, m_ClientID, m_pCharacter->Teams()->TeamMask(m_pCharacter->Team(), -1, m_ClientID));
			GameServer()->CreateSound(m_pCharacter->m_Pos, SOUND_PLAYER_DIE, m_pCharacter->Teams()->TeamMask(m_pCharacter->Team(), -1, m_ClientID));
			m_NextPauseTick = Server()->Tick() + g_Config.m_SvPauseFrequency * Server()->TickSpeed();
		}
	}
	else
	{
		if (m_pCharacter->IsPaused())
		{
			m_pCharacter->Pause(false);
			if (g_Config.m_SvPauseMessages)
			{
				str_format(aBuf, sizeof(aBuf), "'%s' resumed", Server()->ClientName(m_ClientID));
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			}
			GameServer()->CreatePlayerSpawn(m_pCharacter->m_Pos, m_pCharacter->Teams()->TeamMask(m_pCharacter->Team(), -1, m_ClientID));
			m_NextPauseTick = Server()->Tick() + g_Config.m_SvPauseFrequency * Server()->TickSpeed();
		}
	}
}

bool CPlayer::IsPlaying()
{
	if (m_pCharacter && m_pCharacter->IsAlive())
		return true;
	return false;
}

void CPlayer::FindDuplicateSkins()
{
	if (m_TeeInfos.m_UseCustomColor == 0 && !m_StolenSkin) return;
	m_StolenSkin = 0;
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (i == m_ClientID) continue;
		if (GameServer()->m_apPlayers[i])
		{
			if (GameServer()->m_apPlayers[i]->m_StolenSkin) continue;
			if ((GameServer()->m_apPlayers[i]->m_TeeInfos.m_UseCustomColor == m_TeeInfos.m_UseCustomColor) &&
				(GameServer()->m_apPlayers[i]->m_TeeInfos.m_ColorFeet == m_TeeInfos.m_ColorFeet) &&
				(GameServer()->m_apPlayers[i]->m_TeeInfos.m_ColorBody == m_TeeInfos.m_ColorBody) &&
				!str_comp(GameServer()->m_apPlayers[i]->m_TeeInfos.m_SkinName, m_TeeInfos.m_SkinName))
			{
				m_StolenSkin = 1;
				return;
			}
		}
	}
}

void CPlayer::QuestReset()
{
	m_QuestData.Reset();
}

void CPlayer::HandleQuest()
{
	if (!GetCharacter() || !GetCharacter()->IsAlive())
		return;

	if (m_QuestData.m_QuestPart == QUEST_NONE || m_QuestData.m_QuestPart == QUEST_FINISHED || GetCharacter()->Team() != 0)
		return;

	const int OwnID = GetCharacter()->Core()->m_Id;

	if (m_QuestData.m_QuestPart != QUEST_PART_RACE)
	{
		if (!Server()->ClientIngame(m_QuestData.m_VictimID))
		{
			GameServer()->SendChatTarget(OwnID, "[QUEST] Your victim has left the game, selecting a new one");
			m_QuestData.m_QuestPart--;
			QuestSetNextPart();

			return;
		}
	}

	// update special quest parts
	switch (m_QuestData.m_QuestPart)
	{
	case QUEST_PART_RACE:
	{
		if (GetCharacter()->m_DDRaceState == DDRACE_STARTED)
		{
			if (m_QuestData.m_RaceStartTick == 0)
			{
				// he just started the race
				if (g_Config.m_SvQuestRaceTime)
				{
					char aBuf[128];
					str_format(aBuf, sizeof(aBuf), "[QUEST] Now hurry! You got %i minute%s to finish the race!", g_Config.m_SvQuestRaceTime, g_Config.m_SvQuestRaceTime == 1 ? "" : "s");
					GameServer()->SendBroadcast(aBuf, OwnID);
					GameServer()->SendChatTarget(OwnID, aBuf);
				}
				m_QuestData.m_RaceStartTick = Server()->Tick();
			}
			else if (g_Config.m_SvQuestRaceTime && Server()->Tick() > m_QuestData.m_RaceStartTick + g_Config.m_SvQuestRaceTime * 60 * Server()->TickSpeed())
			{
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "[QUEST] Quest failed, you couldn't complete the race in under %i minute%s", g_Config.m_SvQuestRaceTime, g_Config.m_SvQuestRaceTime == 1 ? "" : "s");
				GameServer()->SendChatTarget(OwnID, aBuf);
				QuestReset();
			}
		}
		else if (GetCharacter()->m_DDRaceState == DDRACE_FINISHED && m_QuestData.m_RaceStartTick)
			QuestSetNextPart();
		/*else
		{
		if(m_QuestData.m_RaceStartTick != 0)
		{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "[QUEST] Quest failed, you aborted the race");
		GameServer()->SendBroadcast(aBuf, OwnID);
		GameServer()->SendChatTarget(OwnID, aBuf);
		QuestReset();
		}
		}*/
	} break;
	case QUEST_PART_HOOK:
	{
		if (GetCharacter()->m_Core.m_HookedPlayer == m_QuestData.m_VictimID)
			QuestSetNextPart();
	} break;
	case QUEST_PART_BLOCK:
	{
		CCharacter *pVictim = GameServer()->GetPlayerChar(m_QuestData.m_VictimID);
		if (pVictim && pVictim->IsAlive() && pVictim->Core()->m_LastHookedBy == OwnID && pVictim->m_FirstFreezeTick != 0)
			QuestSetNextPart();
	} break;
	}
}

void CPlayer::QuestTellObjective()
{
	const int OwnID = GetCharacter()->m_Core.m_Id;

	if (m_QuestData.m_QuestPart == QUEST_NONE)
	{
		GameServer()->SendChatTarget(OwnID, "You don't have a quest at the moment. Type /beginquest to start one!");
		return;
	}

	char aMessage[128];
	const char *pVictimName = Server()->ClientName(m_QuestData.m_VictimID);
	switch (m_QuestData.m_QuestPart)
	{
	case QUEST_PART_RACE:
	{
		//KillCharacter(); // They need to reset to start the race //CRAP CRAP CRAP crashbug fix
		if (g_Config.m_SvQuestRaceTime)
			str_format(aMessage, sizeof(aMessage), "Complete the race in less than %i minute%s!", g_Config.m_SvQuestRaceTime, g_Config.m_SvQuestRaceTime == 1 ? "" : "s");
		else
			str_format(aMessage, sizeof(aMessage), "Complete the race!");

		if (m_QuestData.m_RaceStartTick == 0) // Revaliuate
			m_QuestData.m_RaceStartTick++;
	} break;
	case QUEST_PART_BLOCK:
		str_format(aMessage, sizeof(aMessage), "You must block %s!", pVictimName);
		break;
	case QUEST_PART_HOOK:
		str_format(aMessage, sizeof(aMessage), "You must hook %s!", pVictimName);
		break;
	case QUEST_PART_HAMMER:
		str_format(aMessage, sizeof(aMessage), "You must hammer %s!", pVictimName);
		break;
	case QUEST_PART_LASER:
		str_format(aMessage, sizeof(aMessage), "You must shoot %s with laser/rifle!", pVictimName);
		break;
	case QUEST_PART_SHOTGUN:
		str_format(aMessage, sizeof(aMessage), "You must shoot %s with shotgun!", pVictimName);
		break;
	default: // shouldn't happen... and if it does because someone made a dumb mistake, this is a nice easter egg :D
		str_format(aMessage, sizeof(aMessage), "You must tell an admin that there's an error in the program!");
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "[QUEST %i/%i] %s", m_QuestData.m_QuestPart, NUM_QUESTS, aMessage);

	GameServer()->SendBroadcast(aBuf, OwnID);
	GameServer()->SendChatTarget(OwnID, aBuf);

}

void CPlayer::QuestSetNextPart()
{
	if (!GetCharacter())
	{
		dbg_msg("ERROR", "--------------------------------------------------");
		dbg_msg("ERROR", "%s:%i", __FILE__, __LINE__);
		dbg_msg("ERROR", "  CPlayer::QuestSetNextPart called");
		dbg_msg("ERROR", "  but GetCharacter() == NULL ?!");
		dbg_msg("ERROR", "--------------------------------------------------");
		return;
	}

	const int OwnID = GetCharacter()->m_Core.m_Id;

	// advance to the next quest part
	m_QuestData.m_QuestPart++;


	if (m_QuestData.m_QuestPart >= QUEST_FINISHED) // handle finished quest
	{
		GameServer()->SendChatTarget(OwnID, "Congratulations, you received +1 Pages for completing the quest!");
		m_QuestData.m_Pages++;
		QuestReset();

		KillCharacter(); // Looks professional with a death at completion :)
		return;
	}

	// Thats dumb asf Henritees -.- Did you even try there?
	if (m_QuestData.m_QuestPart != QUEST_PART_RACE)
	{
		// count players
		int PlayerCount = 0;
		for (int i = 0; i < MAX_CLIENTS; i++)
		{
			CCharacter *pChr = GameServer()->GetPlayerChar(i);
			if ((pChr && pChr->IsAlive()) &&
				!(!GameServer()->GetPlayerChar(i) ||
					!GameServer()->GetPlayerChar(i)->IsAlive() ||
					GameServer()->GetPlayerChar(i)->Team() != 0 ||
					GameServer()->GetPlayerChar(i)->GetPlayer()->m_Afk))
				PlayerCount++;
		}
		// find out a new victim
		do
		{
			m_QuestData.m_VictimID = rand() % PlayerCount;
		} while (m_QuestData.m_VictimID == OwnID ||
			!GameServer()->GetPlayerChar(m_QuestData.m_VictimID) ||
			!GameServer()->GetPlayerChar(m_QuestData.m_VictimID)->IsAlive() ||
			GameServer()->GetPlayerChar(m_QuestData.m_VictimID)->Team() != 0 ||
			GameServer()->GetPlayerChar(m_QuestData.m_VictimID)->GetPlayer()->m_Afk
			);

		// tell him what to do next
	}

	QuestTellObjective();
}

void CPlayer::SaveStats()
{
	m_SavedStats.m_SavedSpawn = GetCharacter()->Core()->m_Pos;
	m_SavedStats.m_SavedShotgun = GetCharacter()->GetWeaponGot(WEAPON_SHOTGUN);
	m_SavedStats.m_SavedGrenade = GetCharacter()->GetWeaponGot(WEAPON_GRENADE);
	m_SavedStats.m_SavedLaser = GetCharacter()->GetWeaponGot(WEAPON_RIFLE);
	m_SavedStats.m_SavedEHook = GetCharacter()->m_EndlessHook;

	m_SavedStats.m_SavedLovely = m_Lovely;
	m_SavedStats.m_SavedHeartGuns = m_HeartGuns;
	m_SavedStats.m_SavedBall = m_IsBallSpawned;
	m_SavedStats.m_SavedRainbow = m_Rainbow;
	m_SavedStats.m_SavedERainbow = m_Rainbowepiletic;
	m_SavedStats.m_SavedEpicCircle = m_EpicCircle;
	m_SavedStats.m_SavedRainbowHook = m_RainbowHook;

	m_SavedStats.m_SavedHammerHit = GetCharacter()->m_Hit;
	m_SavedStats.m_SavedHook = GetCharacter()->Core()->m_Hook;
	m_SavedStats.m_SavedSolo = GetCharacter()->Teams()->m_Core.GetSolo(GetCID());

	if (GetCharacter()->m_DDRaceState == DDRACE_STARTED)
		m_SavedStats.m_SavedStartTick = GetCharacter()->m_StartTime;
}

void CPlayer::LMBRestore()
{
	if (m_SavedStats.m_SavedShotgun)
		m_pCharacter->GiveWeapon(WEAPON_SHOTGUN);

	if (m_SavedStats.m_SavedGrenade)
		m_pCharacter->GiveWeapon(WEAPON_GRENADE);

	if (m_SavedStats.m_SavedLaser)
		m_pCharacter->GiveWeapon(WEAPON_RIFLE);

	m_Rainbow = m_SavedStats.m_SavedRainbow;
	m_Rainbowepiletic = m_SavedStats.m_SavedERainbow;
	m_Lovely = m_SavedStats.m_SavedLovely;
	m_HeartGuns = m_SavedStats.m_SavedHeartGuns;
	m_RainbowHook = m_SavedStats.m_SavedRainbowHook;
	m_pCharacter->m_EndlessHook = m_SavedStats.m_SavedEHook;

	if (m_SavedStats.m_SavedBall)
	{
		m_IsBallSpawned = true;
		new CBall(&GameServer()->m_World, GetCharacter()->m_Pos, GetCID());
	}
		
	if (m_SavedStats.m_SavedEpicCircle)
	{
		m_EpicCircle = true;
		new CEpicCircle(&GameServer()->m_World, GetCharacter()->m_Pos, GetCID());
	}

	if(m_SavedStats.m_SavedHammerHit)
		GetCharacter()->HandleHit(false);

	if(m_SavedStats.m_SavedHook)
		GetCharacter()->HandleHook(false);

	if(m_SavedStats.m_SavedSolo)
		GetCharacter()->HandleSolo(true);

	if (m_SavedStats.m_SavedStartTick)
	{
		m_pCharacter->Teams()->OnCharacterStart(GetCID());
		m_pCharacter->m_StartTime = m_SavedStats.m_SavedStartTick;
	}

	m_SavedStats.Reset();
}