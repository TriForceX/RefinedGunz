#include "stdafx.h"
#include "ZGame.h"
#include "ZCamera.h"
#include "RealSpace2.h"
#include "RBspObject.h"
#include "ZApplication.h"
#include "ZObserver.h"
#include "ZMatch.h"
#include "ZGameClient.h"
#include "ZMyCharacter.h"
#include "ZCharacter.h"
#include "Config.h"
#include "ZMyInfo.h"
#include "ZGameConst.h"
#include "ZInput.h"
#include "ZConfiguration.h"

#define CAMERA_TRACKSPEED			0.2f
#define CAMERA_WALL_TRACKSPEED		0.7f

ZCamera::ZCamera() : m_fAngleX(CAMERA_DEFAULT_ANGLEX), m_fAngleZ(CAMERA_DEFAULT_ANGLEZ),
m_fDist(CAMERA_DEFAULT_DISTANCE), m_fCurrentDist(CAMERA_DEFAULT_DISTANCE),
m_bShocked(false),
m_Position(0, 0, 0),
m_bAutoAiming(false),
m_nLookMode(ZCAMERA_DEFAULT)
{}

rvector ZCamera::GetCurrentDir()
{
	return rvector(cosf(m_fAngleZ)*sinf(m_fAngleX),
		sinf(m_fAngleZ)*sinf(m_fAngleX),
		cosf(m_fAngleX));
}

static v3 GetTargetOffset(ZCharacter* Char, const v3& Dir)
{
	v3 right;
	CrossProduct(&right, v3{ 0, 0, 1 }, Dir);

	// Height of the camera
	v3 Offset{ 0, 0, 170 };

	// Move to the right, so that we're looking over the player's right shoulder
	Offset += 30.f * right;

	if (Char->GetScale() != 1.0f)
		Offset *= Char->GetScale();

	return Offset;
};

void ZCamera::Update(float fElapsed)
{
	if (!g_pGame)
	{
		assert(false);
		return;
	}

	if (isnan(m_fAngleX) || isnan(m_fAngleZ))
	{
		DMLog("Camera angle is NaN!\n");
		m_fAngleX = CAMERA_DEFAULT_ANGLEX;
		m_fAngleZ = CAMERA_DEFAULT_ANGLEZ;
	}

	ZCombatInterface*	pCombatInterface = ZGetGameInterface()->GetCombatInterface();
	ZCharacter*			pTargetCharacter = g_pGame->m_pMyCharacter;

	v3 PlayerPosition{ pTargetCharacter->m_Position };
	v3 PlayerDirection{ pTargetCharacter->CameraDir };
	bool Observing = false;

	if (pCombatInterface->GetObserver()->IsVisible())
	{
		ZCharacter *pObserverTargetCharacter = pCombatInterface->GetObserver()->GetTargetCharacter();
		if (pObserverTargetCharacter)
		{
			pTargetCharacter = pObserverTargetCharacter;
			Observing = pTargetCharacter != nullptr;
		}

		if (pTargetCharacter)
			pTargetCharacter->GetHistory(&PlayerPosition, nullptr,
				g_pGame->GetTime() - pCombatInterface->GetObserver()->GetDelay(), &PlayerDirection);

		/*DMLog("ZCamera::Update - Time = %f - %f, %f, %f\n",
			g_pGame->GetTime() - pCombatInterface->GetObserver()->GetDelay(),
			PlayerDirection.x, PlayerDirection.y, PlayerDirection.z);*/
	}
	if (!pTargetCharacter) return;

	m_Target = PlayerPosition + GetTargetOffset(pTargetCharacter, PlayerDirection);

	v3 dir;
	if (Observing && GetLookMode() == ZCAMERA_DEFAULT)
	{
		// If we're spectating someone, set the direction to their direction.
		dir = PlayerDirection;

		m_fAngleX = acosf(dir.z);
		v3 HorizontalDir{ dir.x, dir.y, 0.f };
		m_fAngleZ = GetAngleOfVectors(rvector(1, 0, 0), HorizontalDir);
	}
	else
		dir = GetCurrentDir();

	v3 shockoffset{ 0, 0, 0 };
	if (m_bShocked)
	{
		float fA = RANDOMFLOAT * 2 * pi;
		float fB = RANDOMFLOAT * 2 * pi;
		auto velocity = v3(sin(fA)*sin(fB), cos(fA)*sin(fB), cos(fB));

		float fPower = (g_pGame->GetTime() - m_fShockStartTime) / m_fShockDuration;
		if (fPower > 1.f)
		{
			StopShock();
		}
		else
		{
			fPower = 1.f - fPower;

			const float FFMAX_POWER = 300.f;

			float ffPower = fPower * m_fShockPower / FFMAX_POWER;
			ffPower = min(ffPower, 1);
			ZGetInput()->SetDeviceForcesXY(ffPower, ffPower);

			fPower = pow(fPower, 1.5f);
			auto ShockVelocity = RANDOMFLOAT * m_fShockPower * velocity;
			shockoffset = fPower * fElapsed * ShockVelocity;
		}
	}

	float fRealDist = m_fDist;
	rvector pos = m_CurrentTarget - dir*m_fDist;

	RBSPPICKINFO bpi;

	if (ZGetGame()->GetWorld()->GetBsp()->Pick(m_Target, -dir, &bpi)
		&& Magnitude(m_Target - bpi.PickPos) < Magnitude(m_Target - pos))
	{
		float fColDist = Magnitude(bpi.PickPos - pos);
		float fTargetDist = Magnitude(m_Target - pos);
		pos = bpi.PickPos + dir;

		fRealDist = Magnitude(m_Target - pos) - 10.f;
	}

	if (pTargetCharacter && pTargetCharacter->GetScale() != 1.0f)
		fRealDist *= pTargetCharacter->GetScale();

	m_CurrentTarget = m_Target;

	bool bCollisionWall = CheckCollisionWall(fRealDist, pos, dir);

	float fAddedZ = 0.0f;
	CalcMaxPayneCameraZ(fRealDist, fAddedZ, m_fAngleX);

	if (bCollisionWall)
	{
		m_fCurrentDist += CAMERA_WALL_TRACKSPEED*(fRealDist - m_fCurrentDist);
	}
	else
	{
		m_fCurrentDist += CAMERA_TRACKSPEED*(fRealDist - m_fCurrentDist);
	}

	if (GetLookMode() == ZCAMERA_DEFAULT || GetLookMode() == ZCAMERA_FREEANGLE) {

		m_Position = m_CurrentTarget - dir*m_fCurrentDist;
		m_Position.z += fAddedZ;

		// Lock the camera height if the player has fallen into the abyss
		if (m_Position.z <= DIE_CRITICAL_LINE)
		{
			rvector campos = pTargetCharacter->GetCenterPos() - pTargetCharacter->GetDirection() * 20.0f;
			m_Position.x = campos.x;
			m_Position.y = campos.y;
			m_Position.z = DIE_CRITICAL_LINE;

			if (GetLookMode() == ZCAMERA_DEFAULT)
			{
				rvector tar = pTargetCharacter->GetCenterPos();
				if (tar.z < (DIE_CRITICAL_LINE - 1000.0f)) tar.z = DIE_CRITICAL_LINE - 1000.0f;
				dir = tar - m_Position;
				Normalize(dir);
			}
		}
	}

	v3 up{ 0, 0, 1 };
	if (m_bShocked)
	{
		rvector CameraPos = m_Position + shockoffset;
		RSetCamera(CameraPos, CameraPos + dir, up);
	}
	else
	{
		RSetCamera(m_Position, m_Position + dir, up);
	}

	if (GetLookMode() == ZCAMERA_FREELOOK)
	{
		if (!_isnan(RCameraDirection.x) && !_isnan(RCameraDirection.y) && !_isnan(RCameraDirection.z))
		{
			ZGetGameInterface()->GetCombatInterface()->GetObserver()->SetFreeLookTarget(RCameraDirection);
		}
	}
}

void ZCamera::Shock(float fPower, float fDuration, const rvector& vDir)
{
	m_bShocked = true;

	m_fShockStartTime = g_pGame->GetTime();
	m_fShockPower = fPower;
	m_fShockDuration = fDuration;
}

void ZCamera::StopShock()
{
	m_bShocked = false;
	ZGetInput()->SetDeviceForcesXY(0, 0);
}

void ZCamera::SetDirection(const rvector& dir)
{
	rvector a_dir = dir;
	Normalize(a_dir);

	float fAngleX = 0.0f, fAngleZ = 0.0f;

	fAngleX = acosf(a_dir.z);
	float fSinX = sinf(fAngleX);

	if (fSinX == 0) fAngleZ = 0.0f;
	else
	{
		float fT = (a_dir.x / fSinX);
		if (fT > 1.0f) fT = 1.0f;
		else if (fT < -1.0f) fT = -1.0f;

		float fZ1 = acosf(fT);

		if (IS_EQ((sinf(fZ1) * fSinX), dir.y))
		{
			fAngleZ = fZ1;
		}
		else
		{
			fAngleZ = 2 * pi - fZ1;
		}

	}

	m_fAngleX = fAngleX;
	m_fAngleZ = fAngleZ;
}

void ZCamera::Init()
{
	m_fAngleX = CAMERA_DEFAULT_ANGLEX;
	m_fAngleZ = CAMERA_DEFAULT_ANGLEZ;
	m_fDist = CAMERA_DEFAULT_DISTANCE;
	if (ZGetConfiguration()->GetCamFix())
		m_fDist *= RGetScreenWidth() / RGetScreenHeight() / (4.f / 3.f);
	m_fCurrentDist = CAMERA_DEFAULT_DISTANCE;
	m_bShocked = false;
	m_Position = rvector(0, 0, 0);
	m_CurrentTarget = rvector(0, 0, 0);
}

bool ZCamera::CheckCollisionWall(float &fRealDist, rvector& pos, rvector& dir)
{
	RBSPPICKINFO bpi;
	float fNearZ = DEFAULT_NEAR_Z;
	rvector pos2 = pos;							// camera pos
	rvector tarpos = pos2 + (dir * fNearZ);		// near pos

	rvector up2, right2;
	up2 = rvector(0.0f, 0.0f, 1.0f);
	D3DXVec3Cross(&right2, &dir, &up2);
	D3DXVec3Normalize(&right2, &right2);

	D3DXVec3Cross(&up2, &right2, &dir);
	D3DXVec3Normalize(&up2, &up2);
	D3DXVec3Cross(&right2, &dir, &up2);
	D3DXVec3Normalize(&right2, &right2);

	float fov = g_fFOV;
	float e = 1 / (tanf(fov / 2));
	float fAspect = (float)RGetScreenWidth() / (float)RGetScreenHeight();
	float fPV = (fAspect * fNearZ / e);
	float fPH = (fNearZ / e);

	bool bCollisionWall = false;
	rmatrix matView;

	pos2 = pos;
	rvector tar = tarpos + (up2 * fPV);
	rvector dir2;
	dir2 = tar - pos2;
	D3DXVec3Normalize(&dir2, &dir2);
	D3DXMatrixLookAtLH(&matView, &pos2, &dir, &up2);

	if (ZGetGame()->GetWorld()->GetBsp()->Pick(pos2, dir2, &bpi))
	{
		if (Magnitude(tar - bpi.PickPos) < Magnitude(tar - pos2))
		{
			rvector v1, v2, v3, v4;

			v1 = bpi.PickPos;
			v3 = tar;

			if (ZGetGame()->GetWorld()->GetBsp()->Pick(tarpos, up2, &bpi))
			{
				v2 = bpi.PickPos;

				float fD = Magnitude(tarpos - v2);
				if (fD < fPH)
				{
					rvector vv1 = v1 - v2, vv2 = v2 - v3;
					D3DXVECTOR4 rV4;
					D3DXVec3Transform(&rV4, &vv1, &matView);
					vv1.x = rV4.x; vv1.y = rV4.y; vv1.z = rV4.z;
					D3DXVec3Transform(&rV4, &vv2, &matView);
					vv2.x = rV4.x; vv2.y = rV4.y; vv2.z = rV4.z;

					float fAng = GetAngleOfVectors(vv1, vv2);
					if (fAng < 0.0f) fAng = -fAng;

					if (fAng < pi)
					{
						bCollisionWall = true;

						float fX = fPV - fD;
						float fY = fX * tanf(fAng);

						float fMyRealDist = fRealDist - fY;
						fRealDist = min(fMyRealDist, fRealDist);
					}
				}
			}
		}
	}

	pos2 = pos;
	tar = tarpos + (right2 * fPH);
	dir2 = tar - pos2;
	D3DXVec3Normalize(&dir2, &dir2);

	if (ZGetGame()->GetWorld()->GetBsp()->Pick(pos2, dir2, &bpi))
	{
		if (Magnitude(tar - bpi.PickPos) < Magnitude(tar - pos2))
		{
			rvector v1, v2, v3, v4;

			v1 = bpi.PickPos;
			v3 = tar;

			if (ZGetGame()->GetWorld()->GetBsp()->Pick(tarpos, right2, &bpi))
			{
				v2 = bpi.PickPos;

				float fD = Magnitude(tarpos - v2);
				if (fD < fPH)
				{
					rvector vv1 = v1 - v2, vv2 = v2 - v3;
					float fAng = GetAngleOfVectors(vv1, vv2);
					if (fAng < 0.0f) fAng = -fAng;

					if (fAng < (pi / 2))
					{
						bCollisionWall = true;

						float fX = fPH - fD;
						float fY = fX * tanf(fAng);

						float fMyRealDist = fRealDist - fY;
						fRealDist = min(fMyRealDist, fRealDist);
					}
				}
			}
		}
	}

	pos2 = pos;
	tar = tarpos - (up2 * fPV);
	dir2 = tar - pos2;
	D3DXVec3Normalize(&dir2, &dir2);
	D3DXMatrixLookAtLH(&matView, &pos2, &dir2, &up2);

	if (ZGetGame()->GetWorld()->GetBsp()->Pick(pos2, dir2, &bpi))
	{
		if (Magnitude(tar - bpi.PickPos) < Magnitude(tar - pos2))
		{
			rvector v1, v2, v3, v4;

			v1 = bpi.PickPos;
			v3 = tar;

			if (ZGetGame()->GetWorld()->GetBsp()->Pick(tarpos, -up2, &bpi))
			{
				v2 = bpi.PickPos;

				float fD = Magnitude(tarpos - v2);
				if (fD < fPH)
				{
					bCollisionWall = true;

					rvector vv1 = v1 - v2, vv2 = v2 - v3;
					D3DXVECTOR4 rV4;
					D3DXVec3Transform(&rV4, &vv1, &matView);
					vv1.x = rV4.x; vv1.y = rV4.y; vv1.z = rV4.z;
					D3DXVec3Transform(&rV4, &vv2, &matView);
					vv2.x = rV4.x; vv2.y = rV4.y; vv2.z = rV4.z;


					float fAng = GetAngleOfVectors(vv1, vv2);
					if (fAng < 0.0f) fAng = -fAng;

					if (fAng < (pi / 2))
					{
						float fX = fPV - fD;
						float fY = fX * tanf(fAng);

						float fMyRealDist = fRealDist - fY;
						fRealDist = min(fMyRealDist, fRealDist);
					}
				}
			}
		}
	}

	pos2 = pos;
	tar = tarpos - (right2 * fPH);
	dir2 = tar - pos2;
	D3DXVec3Normalize(&dir2, &dir2);

	if (ZGetGame()->GetWorld()->GetBsp()->Pick(pos2, dir2, &bpi))
	{
		if (Magnitude(tar - bpi.PickPos) < Magnitude(tar - pos2))
		{
			rvector v1, v2, v3, v4;

			v1 = bpi.PickPos;
			v3 = tar;

			if (ZGetGame()->GetWorld()->GetBsp()->Pick(tarpos, -right2, &bpi))
			{
				v2 = bpi.PickPos;

				float fD = Magnitude(tarpos - v2);
				if (fD < fPH)
				{
					bCollisionWall = true;

					rvector vv1 = v1 - v2, vv2 = v2 - v3;
					float fAng = GetAngleOfVectors(vv1, vv2);
					if (fAng < 0.0f) fAng = -fAng;

					if (fAng < (pi / 2))
					{
						float fX = fPH - fD;
						float fY = fX * tanf(fAng);

						float fMyRealDist = fRealDist - fY;
						fRealDist = min(fMyRealDist, fRealDist);
					}
				}
			}
		}
	}

	if (fRealDist < 0) fRealDist = 0.0f;

	return bCollisionWall;
}

void ZCamera::CalcMaxPayneCameraZ(float &fRealDist, float& fAddedZ, float fAngleX)
{
	ZMyCharacter* pMyCharacter = ZGetGameInterface()->GetGame()->m_pMyCharacter;

	float fPayneDist = fRealDist;

	if (fAngleX < pi / 2.f)
	{
		float fOffset = (pi / 2.f - fAngleX) / (pi / 2.f);
		float fOffset2 = 1.0f - sinf(((pi / 4.0f) * fOffset));
		fPayneDist = fOffset2 * (m_fDist - 80.0f) + 80.0f;
	}

	if (fAngleX > pi / 2.f)
	{
		float fOffset = (fAngleX - pi / 2.f) / (pi / 2.f);
		float fOffset2 = 1.0f - (cosf(pi + ((pi / 2.0f) * fOffset)) + 1.0f);
		fPayneDist = fOffset2 * (m_fDist - 100.0f) + 100.0f;
	}

	if (fAngleX < 1.3f)
	{
		float fOffset = (1.3f - fAngleX) / 1.3f;
		fOffset = fOffset * (pi / 2.0f);
		float fOffset2 = 1.0f + sinf(pi + (pi / 2.0f) + fOffset);
		fAddedZ = fOffset * 103.0f;
	}

	fRealDist = min(fRealDist, fPayneDist);
}

void ZCamera::SetLookMode(ZCAMERALOOKMODE mode)
{
	if (mode == ZCAMERA_FREELOOK) {
		ZObserver* pObserver = ZApplication::GetGameInterface()->GetCombatInterface()->GetObserver();
		assert(pObserver->IsVisible() && pObserver->GetTargetCharacter() != NULL);

		ZCharacter *pTargetCharacter = pObserver->GetTargetCharacter();
		rvector a_pos;
		rvector a_dir;
		bool bHistory = pTargetCharacter->GetHistory(&a_pos, &a_dir, g_pGame->GetTime() - pObserver->GetDelay());
		assert(bHistory);

		rvector newPos = a_pos + rvector(0, 0, 140);

		rvector targetPosition;
		if (m_nLookMode == ZCAMERA_MINIMAP)
			targetPosition = m_Target - m_fCurrentDist * a_dir;
		else
			targetPosition = m_Position;

		ZGetGame()->GetWorld()->GetBsp()->CheckWall(newPos, targetPosition,
			ZFREEOBSERVER_RADIUS + 1, 0.f, RCW_SPHERE);

		SetPosition(targetPosition);

		bool bCameraInSolid2 = ZGetGame()->GetWorld()->GetBsp()->CheckSolid(GetPosition(),
			ZFREEOBSERVER_RADIUS, 0.f, RCW_SPHERE);
		assert(!bCameraInSolid2);
	}

	m_nLookMode = mode;

	if (mode == ZCAMERA_MINIMAP) {
		if (!ZGetGameInterface()->OpenMiniMap())
			SetNextLookMode();
	}
}

void ZCamera::SetNextLookMode()
{
	ZCAMERALOOKMODE mode = ZCAMERALOOKMODE(m_nLookMode + 1);
	if (mode >= ZCAMERA_FREELOOK)
		mode = ZCAMERA_DEFAULT;
	SetLookMode(mode);
}