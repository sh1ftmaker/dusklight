/**
 * m_Do_controller_pad.cpp
 * JUTGamePad Wrapper and Conversion
 */

#include "m_Do/m_Do_controller_pad.h"
#include "JSystem/JAWExtSystem/JAWExtSystem.h"
#include "SSystem/SComponent/c_lib.h"
#include "d/d_com_inf_game.h"
#include "f_ap/f_ap_game.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_main.h"
#include "dusk/online.h"
#include "dusk/test_input.h"
#include "SSystem/SComponent/c_math.h"
#include "tracy/Tracy.hpp"
#include <cmath>

JUTGamePad* mDoCPd_c::m_gamePad[4];

interface_of_controller_pad mDoCPd_c::m_cpadInfo[4];
interface_of_controller_pad mDoCPd_c::m_debugCpadInfo[4];

void mDoCPd_c::create() {
    #if PLATFORM_GCN || PLATFORM_SHIELD
    m_gamePad[0] = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
    #endif

    if (DEBUG || mDoMain::developmentMode != 0) {
        #if PLATFORM_WII
        m_gamePad[0] = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
        #endif

        m_gamePad[1] = JKR_NEW JUTGamePad(JUTGamePad::EPort2);
        m_gamePad[2] = JKR_NEW JUTGamePad(JUTGamePad::EPort3);
        m_gamePad[3] = JKR_NEW JUTGamePad(JUTGamePad::EPort4);
    } else {
        #if PLATFORM_WII
        m_gamePad[0] = NULL;
        #endif

        m_gamePad[1] = NULL;
        m_gamePad[2] = NULL;
        m_gamePad[3] = NULL;
    }

    #if PLATFORM_GCN || PLATFORM_SHIELD
    if (!mDoRst::isReset()) {
        JUTGamePad::clearResetOccurred();
        JUTGamePad::setResetCallback(mDoRst_resetCallBack, NULL);
    }
    #endif
    JUTGamePad::setAnalogMode(3);

    interface_of_controller_pad* cpad = &m_cpadInfo[0];
    for (int i = 0; i < 4; i++) {
        cpad->mHoldLockL = cpad->mTrigLockL = false;
        cpad->mHoldLockR = cpad->mTrigLockR = false;
        cpad++;
    }
}

void mDoCPd_c::read() {
    ZoneScoped;
    JUTGamePad::read();

    if (!mDoRst::isReset() && mDoRst::is3ButtonReset()) {
        if (!JUTGamePad::getGamePad(mDoRst::get3ButtonResetPort())->isPushing3ButtonReset()) {
            mDoRst::off3ButtonReset();
        }
    }

#if DEBUG
    if (m_gamePad[3]) {
        JAWExtSystem::padProc(*m_gamePad[3]);
    }
#endif

    JUTGamePad** pad = m_gamePad;
    interface_of_controller_pad* interface = m_cpadInfo;
#if DEBUG
    interface_of_controller_pad* interface2 = m_debugCpadInfo;
    if (dComIfG_isDebugMode()) {
        interface_of_controller_pad* tmp = interface;
        interface = interface2;
        interface2 = tmp;
    }
#endif

    for (u32 i = 0; i < 4; i++) {
        if (*pad == NULL) {
            cLib_memSet(interface, 0, sizeof(interface_of_controller_pad));
        } else {
            convert(interface, *pad);
            LRlockCheck(interface);
        }
#if DEBUG
        cLib_memSet(interface2, 0, sizeof(interface_of_controller_pad));
#endif
        pad++;
        interface++;
#if DEBUG
        interface2++;
#endif
    }

    // Online: stream the local player's input to the peer each tick, and inject
    // the peer's most recent input into the second pad slot. The puppet that
    // makes the two players visible to each other is driven from the streamed
    // transform (see m_Do_graphic.cpp); the input channel is carried here so the
    // eventual input-lockstep path has the data it needs.
    // Test harness: overlay synthetic input onto pad slot 0 so an instance can be
    // driven programmatically over UDP (independently per instance). Applied
    // before the online publish so the peer receives the driven input too.
    if (dusk::test_input::active()) {
        dusk::test_input::InputState s;
        if (dusk::test_input::get_state(&s)) {
            static u32 s_prevButtons = 0;
            interface_of_controller_pad& p = m_cpadInfo[0];
            p.mButtonFlags = s.buttons;
            p.mPressedButtonFlags = s.buttons & ~s_prevButtons;
            s_prevButtons = s.buttons;

            f32 mag = sqrtf(s.stickX * s.stickX + s.stickY * s.stickY);
            if (mag > 1.0f) mag = 1.0f;
            p.mMainStickPosX = s.stickX;
            p.mMainStickPosY = s.stickY;
            p.mMainStickValue = mag;
            p.mMainStickAngle = cM_atan2s(s.stickX, s.stickY);

            f32 cmag = sqrtf(s.cStickX * s.cStickX + s.cStickY * s.cStickY);
            if (cmag > 1.0f) cmag = 1.0f;
            p.mCStickPosX = s.cStickX;
            p.mCStickPosY = s.cStickY;
            p.mCStickValue = cmag;
            p.mCStickAngle = cM_atan2s(s.cStickX, s.cStickY);

            p.mTriggerLeft = s.triggerL;
            p.mTriggerRight = s.triggerR;
            LRlockCheck(&p);
        }
    }

    if (dusk::online::is_active()) {
        dusk::online::set_local_input(&m_cpadInfo[0]);
        const dusk::online::PlayerState* rp = dusk::online::remote_player(0);
        if (rp != nullptr) {
            interface_of_controller_pad remote;
            if (dusk::online::get_remote_input(rp->id, &remote)) {
                m_cpadInfo[1] = remote;
            }
        }
    }
}

void mDoCPd_c::convert(interface_of_controller_pad* pInterface, JUTGamePad* pPad) {
    pInterface->mButtonFlags = pPad->getButton();
    pInterface->mPressedButtonFlags = pPad->getTrigger();
    pInterface->mMainStickPosX = pPad->getMainStickX();
    pInterface->mMainStickPosY = pPad->getMainStickY();
    pInterface->mMainStickValue = pPad->getMainStickValue();
    pInterface->mMainStickAngle = pPad->getMainStickAngle();
    pInterface->mCStickPosX = pPad->getSubStickX();
    pInterface->mCStickPosY = pPad->getSubStickY();
    pInterface->mCStickValue = pPad->getSubStickValue();
    pInterface->mCStickAngle = pPad->getSubStickAngle();

    mDoCPd_ANALOG_CONV(pPad->getAnalogA(), pInterface->mAnalogA);
    mDoCPd_ANALOG_CONV(pPad->getAnalogB(), pInterface->mAnalogB);
    mDoCPd_TRIGGER_CONV(pPad->getAnalogL(), pInterface->mTriggerLeft);
    mDoCPd_TRIGGER_CONV(pPad->getAnalogR(), pInterface->mTriggerRight);

    pInterface->mGamepadErrorFlags = pPad->getErrorStatus();
}

void mDoCPd_c::LRlockCheck(interface_of_controller_pad* interface) {
    f32 trigger = interface->mTriggerLeft;
    interface->mTrigLockL = false;
    interface->mTrigLockR = false;

    if (trigger > fapGmHIO_getLROnValue()) {
        if (interface->mHoldLockL != true) {
            interface->mTrigLockL = true;
        }
        interface->mHoldLockL = true;
    } else if (trigger < fapGmHIO_getLROffValue()) {
        interface->mHoldLockL = false;
    }

    trigger = interface->mTriggerRight;
    if (trigger > fapGmHIO_getLROnValue()) {
        if (interface->mHoldLockR != true) {
            interface->mTrigLockR = true;
        }
        interface->mHoldLockR = true;
    } else if (trigger < fapGmHIO_getLROffValue()) {
        interface->mHoldLockR = false;
    }
}

void mDoCPd_c::recalibrate(void) {
    JUTGamePad::clearForReset();
    JUTGamePad::CRumble::setEnabled(PAD_CHAN3_BIT | PAD_CHAN2_BIT | PAD_CHAN1_BIT | PAD_CHAN0_BIT);
}
