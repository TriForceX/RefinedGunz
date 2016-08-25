#include "stdafx.h"
#include "MMatchServer.h"
#include "MSharedCommandTable.h"
#include "MErrorTable.h"
#include "MBlobArray.h"
#include "MObject.h"
#include "MMatchObject.h"
#include "Msg.h"
#include "MMatchConfig.h"
#include "MCommandCommunicator.h"
#include "MDebug.h"
#include "MMatchAuth.h"
#include "MAsyncDBJob.h"
#include "MAsyncDBJob_GetLoginInfo.h"
#include "MAsyncDBJob_InsertConnLog.h"
#include "RTypes.h"
#include "MMatchUtil.h"
#include <winbase.h>
#include "MMatchPremiumIPCache.h"
#include "MCommandBuilder.h"
#include "MMatchStatus.h"
#include "MMatchLocale.h"

#define SODIUM_STATIC
#include "sodium.h"

bool MMatchServer::CheckOnLoginPre(const MUID& CommUID, int nCmdVersion, bool& outbFreeIP, string& strCountryCode3)
{
	MCommObject* pCommObj = (MCommObject*)m_CommRefCache.GetRef(CommUID);
	if (pCommObj == NULL) return false;

	// �������� ���� üũ
	if (nCmdVersion != MCOMMAND_VERSION)
	{
		MCommand* pCmd = CreateCmdMatchResponseLoginFailed(CommUID, MERR_COMMAND_INVALID_VERSION);
		Post(pCmd);	
		return false;
	}

	// free login ip�� �˻��ϱ����� debug������ debug ip�� �˻��Ѵ�.
	// ������ debugŸ������ �˻�.
	if( MGetServerConfig()->IsDebugServer() && MGetServerConfig()->IsDebugLoginIPList(pCommObj->GetIPString()) )
	{
		outbFreeIP = true;
		return true;
	}

	// �ִ��ο� üũ
	bool bFreeLoginIP = false;
	if (MGetServerConfig()->CheckFreeLoginIPList(pCommObj->GetIPString()) == true) {
		bFreeLoginIP = true;
		outbFreeIP = true;
		return true;
	} else {
		outbFreeIP = false;

		if ((int)m_Objects.size() >= MGetServerConfig()->GetMaxUser())
		{
			MCommand* pCmd = CreateCmdMatchResponseLoginFailed(CommUID, MERR_CLIENT_FULL_PLAYERS);
			Post(pCmd);	
			return false;
		}
	}

	if( CheckIsValidIP(CommUID, pCommObj->GetIPString(), strCountryCode3, MGetServerConfig()->IsUseFilter()) )
		IncreaseNonBlockCount();
	else
	{
		IncreaseBlockCount();
		return false;
	}

	return true;
}

void MMatchServer::OnMatchLogin(MUID CommUID, const char* szUserID, const unsigned char *HashedPassword, int HashLength, int nCommandVersion, unsigned long nChecksumPack, int nVersion)
{
	if (HashLength != crypto_generichash_BYTES)
		return;

	int nMapID = 0;

	unsigned int nAID = 0;
	char szDBPassword[256]{};
	std::string strCountryCode3;

	bool bFreeLoginIP = false;

	if (nVersion < RGUNZ_VERSION)
	{
		char buf[128];
		sprintf_safe(buf, "Your client is outdated (expected version %d, got %d)", RGUNZ_VERSION, nVersion);
		NotifyFailedLogin(CommUID, buf);
		return;
	}

	if (!CheckOnLoginPre(CommUID, nCommandVersion, bFreeLoginIP, strCountryCode3)) return;

	if (!GetDBMgr()->GetLoginInfo(szUserID, &nAID, szDBPassword))
	{
		char buf[128];
		sprintf_safe(buf, "Couldn't find username %s", szUserID);
		NotifyFailedLogin(CommUID, buf);

		return;
	}

	MCommObject* pCommObj = (MCommObject*)m_CommRefCache.GetRef(CommUID);
	if (pCommObj)
	{
		// ��� ���� ���ӽð��� ������Ʈ �Ѵ�.
		if (!GetDBMgr()->UpdateLastConnDate(szUserID, pCommObj->GetIPString()))
		{
			mlog("DB Query(OnMatchLogin > UpdateLastConnDate) Failed");
		}

	}

	if (crypto_pwhash_scryptsalsa208sha256_str_verify
		(szDBPassword, (char *)HashedPassword, HashLength) != 0) {
		MCommand* pCmd = CreateCmdMatchResponseLoginFailed(CommUID, MERR_CLIENT_WRONG_PASSWORD);
		Post(pCmd);
		return;
	}

	MMatchAccountInfo accountInfo;
	if (!GetDBMgr()->GetAccountInfo(nAID, &accountInfo))
	{
		NotifyFailedLogin(CommUID, "Failed to retrieve account information");
		Disconnect(CommUID);
		return;
	}

#ifndef _DEBUG
	MMatchObject* pCopyObj = GetPlayerByAID(accountInfo.m_nAID);
	if (pCopyObj != NULL) 
	{
		DisconnectObject(pCopyObj->GetUID());
	}
#endif

	if ((accountInfo.m_nUGrade == MMUG_BLOCKED) || (accountInfo.m_nUGrade == MMUG_PENALTY))
	{
		MCommand* pCmd = CreateCmdMatchResponseLoginFailed(CommUID, MERR_CLIENT_MMUG_BLOCKED);
		Post(pCmd);
		return;
	}

	AddObjectOnMatchLogin(CommUID, &accountInfo, bFreeLoginIP, strCountryCode3, nChecksumPack);
}

void MMatchServer::NotifyFailedLogin(const MUID& uidComm, const char *szReason)
{
	MCommand* pCmd = CreateCommand(MC_MATCH_RESPONSE_LOGIN_FAILED, uidComm);
	pCmd->AddParameter(new MCommandParameterString(szReason));
	Post(pCmd);
}

void MMatchServer::CreateAccount(const MUID &uidComm, const char *Username,
	const unsigned char *HashedPassword, int HashLength, const char *Email)
{
	if (Username[0] == 0)
	{
		CreateAccountResponse(uidComm, "Account creation failed: Empty username");
		return;
	}

	if (HashLength != crypto_generichash_BYTES)
	{
		CreateAccountResponse(uidComm, "Account creation failed: Invalid hash");
		return;
	}

	char PasswordData[crypto_pwhash_scryptsalsa208sha256_STRBYTES];

	if (crypto_pwhash_scryptsalsa208sha256_str
		(PasswordData, (char *)HashedPassword, HashLength,
		crypto_pwhash_scryptsalsa208sha256_OPSLIMIT_INTERACTIVE,
		crypto_pwhash_scryptsalsa208sha256_MEMLIMIT_INTERACTIVE) != 0) {
		CreateAccountResponse(uidComm, "Account creation failed: Server ran out of memory");
		return;
	}

	auto ret = GetDBMgr()->CreateAccountNew(Username, PasswordData, ArraySize(PasswordData), Email);
	switch (ret)
	{
	case AccountCreationResult::Success:
		CreateAccountResponse(uidComm, "Account created!");
		break;
	case AccountCreationResult::UsernameAlreadyExists:
		CreateAccountResponse(uidComm, "Account creation failed: Username already exists");
		break;
	case AccountCreationResult::DBError:
		CreateAccountResponse(uidComm, "Account creation failed: Unknown database error");
		break;
	default:
		CreateAccountResponse(uidComm, "Account creation failed: Unknown error");
		break;
	};
}

void MMatchServer::CreateAccountResponse(const MUID& uidComm, const char *szReason)
{
	MCommand* pCmd = CreateCommand(MC_MATCH_RESPONSE_CREATE_ACCOUNT, uidComm);
	pCmd->AddParameter(new MCommandParameterString(szReason));
	Post(pCmd);
}

void MMatchServer::OnMatchLoginFromNetmarble(const MUID& CommUID, const char* szCPCookie, const char* szSpareData, int nCmdVersion, unsigned long nChecksumPack)
{
	MCommObject* pCommObj = (MCommObject*)m_CommRefCache.GetRef(CommUID);
	if (pCommObj == NULL) return;

	bool bFreeLoginIP = false;
	std::string strCountryCode3;

	if (!CheckOnLoginPre(CommUID, nCmdVersion, bFreeLoginIP, strCountryCode3)) return;

	MMatchAuthBuilder* pAuthBuilder = GetAuthBuilder();
	if (pAuthBuilder == NULL) {
		LOG(LOG_ALL, "Critical Error : MatchAuthBuilder is not assigned.");
		return;
	}
	MMatchAuthInfo* pAuthInfo = NULL;
	if (pAuthBuilder->ParseAuthInfo(szCPCookie, &pAuthInfo) == false) 
	{
		MGetServerStatusSingleton()->SetRunStatus(5);

		MCommand* pCmd = CreateCmdMatchResponseLoginFailed(CommUID, MERR_CLIENT_WRONG_PASSWORD);
		Post(pCmd);	

		LOG(LOG_ALL, "Netmarble Certification Failed");
		return;
	}

	const char* pUserID = pAuthInfo->GetUserID();
	const char* pUniqueID = pAuthInfo->GetUniqueID();
	const char* pCertificate = pAuthInfo->GetCertificate();
	const char* pName = pAuthInfo->GetName();
	int nAge = pAuthInfo->GetAge();
	int nSex = pAuthInfo->GetSex();
	bool bCheckPremiumIP = MGetServerConfig()->CheckPremiumIP();
	const char* szIP = pCommObj->GetIPString();
	DWORD dwIP = pCommObj->GetIP();

	// Async DB
	MAsyncDBJob_GetLoginInfo* pNewJob = new MAsyncDBJob_GetLoginInfo(CommUID);
	pNewJob->Input(new MMatchAccountInfo(), 
					pUserID, 
					pUniqueID, 
					pCertificate, 
					pName, 
					nAge, 
					nSex, 
					bFreeLoginIP, 
					nChecksumPack,
					bCheckPremiumIP,
					szIP,
					dwIP,
					strCountryCode3);
	PostAsyncJob(pNewJob);

	if (pAuthInfo)
	{
		delete pAuthInfo; pAuthInfo = NULL;
	}
}


void MMatchServer::OnMatchLoginFromNetmarbleJP(const MUID& CommUID, const char* szLoginID, const char* szLoginPW, int nCmdVersion, unsigned long nChecksumPack)
{
	bool bFreeLoginIP = false;
	string strCountryCode3;

	// ��������, �ִ��ο� üũ
	if (!CheckOnLoginPre(CommUID, nCmdVersion, bFreeLoginIP, strCountryCode3)) return;

	// DBAgent�� ���� ������ ������ ������ �α��� ���μ����� �����Ѵ�.
	if (!MGetLocale()->PostLoginInfoToDBAgent(CommUID, szLoginID, szLoginPW, bFreeLoginIP, nChecksumPack, GetClientCount()))
	{
		MCommand* pCmd = CreateCmdMatchResponseLoginFailed(CommUID, MERR_CLIENT_FULL_PLAYERS);
		Post(pCmd);
		return;
	}
}

void MMatchServer::OnMatchLoginFromDBAgent(const MUID& CommUID, const char* szLoginID, const char* szName, int nSex, bool bFreeLoginIP, unsigned long nChecksumPack)
{
	MCommObject* pCommObj = (MCommObject*)m_CommRefCache.GetRef(CommUID);
	if (pCommObj == NULL) return;

	string strCountryCode3;
	CheckIsValidIP( CommUID, pCommObj->GetIPString(), strCountryCode3, false );

	const char* pUserID = szLoginID;
	char szPassword[16] = "";			// �н������ ����
	char szCertificate[16] = "";
	const char* pName = szName;
	int nAge = 20;

	bool bCheckPremiumIP = MGetServerConfig()->CheckPremiumIP();
	const char* szIP = pCommObj->GetIPString();
	DWORD dwIP = pCommObj->GetIP();

	// Async DB
	MAsyncDBJob_GetLoginInfo* pNewJob = new MAsyncDBJob_GetLoginInfo(CommUID);
	pNewJob->Input(new MMatchAccountInfo(), 
					pUserID, 
					szPassword, 
					szCertificate, 
					pName, 
					nAge, 
					nSex, 
					bFreeLoginIP, 
					nChecksumPack,
					bCheckPremiumIP,
					szIP,
					dwIP,
					strCountryCode3);
	PostAsyncJob(pNewJob);
}

void MMatchServer::OnMatchLoginFailedFromDBAgent(const MUID& CommUID, int nResult)
{
	// �������� ���� üũ
	MCommand* pCmd = CreateCmdMatchResponseLoginFailed(CommUID, nResult);
	Post(pCmd);	
}

MCommand* MMatchServer::CreateCmdMatchResponseLoginOK(const MUID& uidComm, 
													  MUID& uidPlayer, 
													  const char* szUserID, 
													  MMatchUserGradeID nUGradeID, 
													  MMatchPremiumGradeID nPGradeID,
													  const char* szRandomValue,
													  const unsigned char* pbyGuidReqMsg)
{
	MCommand* pCmd = CreateCommand(MC_MATCH_RESPONSE_LOGIN, uidComm);
	pCmd->AddParameter(new MCommandParameterInt(MOK));
	pCmd->AddParameter(new MCommandParameterString(MGetServerConfig()->GetServerName()));
	pCmd->AddParameter(new MCommandParameterChar((char)MGetServerConfig()->GetServerMode()));
	pCmd->AddParameter(new MCommandParameterString(szUserID));
	pCmd->AddParameter(new MCommandParameterUChar((unsigned char)nUGradeID));
	pCmd->AddParameter(new MCommandParameterUChar((unsigned char)nPGradeID));
	pCmd->AddParameter(new MCommandParameterUID(uidPlayer));
	pCmd->AddParameter(new MCommandParameterString(szRandomValue));
	
	void* pBlob = MMakeBlobArray(sizeof(unsigned char), SIZEOF_GUIDREQMSG);
	unsigned char* pCmdBlock = (unsigned char*)MGetBlobArrayElement(pBlob, 0);
	CopyMemory(pCmdBlock, pbyGuidReqMsg, SIZEOF_GUIDREQMSG);

	pCmd->AddParameter(new MCommandParameterBlob(pBlob, MGetBlobArraySize(pBlob)));
	MEraseBlobArray(pBlob);

	return pCmd;
}

MCommand* MMatchServer::CreateCmdMatchResponseLoginFailed(const MUID& uidComm, const int nResult)
{
	MCommand* pCmd = CreateCommand(MC_MATCH_RESPONSE_LOGIN, uidComm);
	pCmd->AddParameter(new MCommandParameterInt(nResult));
	pCmd->AddParameter(new MCommandParameterString(MGetServerConfig()->GetServerName()));
	pCmd->AddParameter(new MCommandParameterChar((char)MGetServerConfig()->GetServerMode()));
	pCmd->AddParameter(new MCommandParameterString("Ana"));
	pCmd->AddParameter(new MCommandParameterUChar((unsigned char)MMUG_FREE));
	pCmd->AddParameter(new MCommandParameterUChar((unsigned char)MMPG_FREE));
	pCmd->AddParameter(new MCommandParameterUID(MUID(0,0)));
	pCmd->AddParameter(new MCommandParameterString("A"));
	
	unsigned char tmp = 0;
	void* pBlob = MMakeBlobArray(sizeof(unsigned char), sizeof(unsigned char));
	unsigned char* pCmdBlock = (unsigned char*)MGetBlobArrayElement(pBlob, 0);
	CopyMemory(pCmdBlock, &tmp, sizeof(unsigned char));

	pCmd->AddParameter(new MCommandParameterBlob(pBlob, MGetBlobArraySize(pBlob)));
	MEraseBlobArray(pBlob);

	return pCmd;
}


bool MMatchServer::AddObjectOnMatchLogin(const MUID& uidComm, 
 										 MMatchAccountInfo* pSrcAccountInfo,
 										 bool bFreeLoginIP,
										 string strCountryCode3,
										 unsigned long nChecksumPack)
{
	MCommObject* pCommObj = (MCommObject*)m_CommRefCache.GetRef(uidComm);
	if (pCommObj == NULL) return false;

	MUID AllocUID = uidComm;
	int nErrCode = ObjectAdd(uidComm);
	if(nErrCode!=MOK){
		LOG(LOG_DEBUG, MErrStr(nErrCode) );
	}

	MMatchObject* pObj = GetObject(AllocUID);
	if (pObj == NULL) 
	{
		Disconnect(uidComm);
		return false;
	}

	pObj->AddCommListener(uidComm);
	pObj->SetObjectType(MOT_PC);
	memcpy(pObj->GetAccountInfo(), pSrcAccountInfo, sizeof(MMatchAccountInfo));
	pObj->SetFreeLoginIP(bFreeLoginIP);
	pObj->SetCountryCode3( strCountryCode3 );
	pObj->UpdateTickLastPacketRecved();
	pObj->UpdateLastHShieldMsgRecved();

	if (pCommObj != NULL)
	{
		pObj->SetPeerAddr(pCommObj->GetIP(), pCommObj->GetIPString(), pCommObj->GetPort());
	}
	
	SetClientClockSynchronize(uidComm);

	// �����̾� IP�� üũ�Ѵ�.
	if (MGetServerConfig()->CheckPremiumIP())
	{
		if (pCommObj)
		{
			bool bIsPremiumIP = false;
			bool bExistPremiumIPCache = false;
			
			bExistPremiumIPCache = MPremiumIPCache()->CheckPremiumIP(pCommObj->GetIP(), bIsPremiumIP);

			// ���� ĳ���� ������ ���� DB���� ã���� �Ѵ�.
			if (!bExistPremiumIPCache)
			{
				if (GetDBMgr()->CheckPremiumIP(pCommObj->GetIPString(), bIsPremiumIP))
				{
					// ����� ĳ���� ����
					MPremiumIPCache()->AddIP(pCommObj->GetIP(), bIsPremiumIP);
				}
				else
				{
					MPremiumIPCache()->OnDBFailed();
				}

			}

			if (bIsPremiumIP) pObj->GetAccountInfo()->m_nPGrade = MMPG_PREMIUM_IP;
		}		
	}

#ifdef _HSHIELD
	if( MGetServerConfig()->IsUseHShield() )
	{
		ULONG dwRet = HShield_MakeGuidReqMsg(pObj->GetHShieldInfo()->m_pbyGuidReqMsg, pObj->GetHShieldInfo()->m_pbyGuidReqInfo);

		if(dwRet != ERROR_SUCCESS)
			LOG(LOG_FILE, "@MakeGuidReqMsg - %s Making Guid Req Msg Failed. (Error code = %x)", pObj->GetAccountName(), dwRet);
	}
#endif

#ifdef _XTRAP
	// XTrap Random Value ����
	if( MGetServerConfig()->IsUseXTrap() )
	{
		if (!MGetLocale()->SkipCheckAntiHackCrack())
			XTrap_RandomKeyGenW(pObj->GetAntiHackInfo()->m_szRandomValue);
	}
#endif

	MCommand* pCmd = CreateCmdMatchResponseLoginOK(uidComm, 
												   AllocUID, 
												   pObj->GetAccountInfo()->m_szUserID,
												   pObj->GetAccountInfo()->m_nUGrade,
                                                   pObj->GetAccountInfo()->m_nPGrade,
												   pObj->GetAntiHackInfo()->m_szRandomValue,
												   pObj->GetHShieldInfo()->m_pbyGuidReqMsg);
	Post(pCmd);	

	MAsyncDBJob_InsertConnLog* pNewJob = new MAsyncDBJob_InsertConnLog();
	pNewJob->Input(pObj->GetAccountInfo()->m_nAID, pObj->GetIPString(), pObj->GetCountryCode3() );
	PostAsyncJob(pNewJob);

	return true;
}