/**
 * online/puppet.cpp — remote-player puppet rendering.
 *
 * Extracted from the decompiled d_a_alink.cpp so the actor only carries thin hook
 * calls (the joint-callback hooks + a single update_and_draw call). All puppet
 * logic — pose injection during calc(), the body/head/hands/face sub-models,
 * per-form TEV shading, and the projected shadow — lives here.
 *
 * The local Link's joint callbacks (in d_a_alink.cpp) share model data with the
 * puppets, so during a puppet calc() they route through body_joint_hook /
 * head_joint_hook, which inject the streamed pose and suppress the local-player
 * joint control that would otherwise corrupt the puppet (and the local player).
 */

#include "d/d_com_inf_game.h"   // dComIfG_Bgsp, dComIfGd_setShadow, dComIfGd_addRealShadow
#include "d/d_kankyo.h"          // g_env_light, settingTevStruct, setLightTevColorType_MAJI
#include "d/d_kankyo_tev_str.h"  // dKy_tevstr_c
#include "d/d_drawlist.h"        // dDlst_shadowControl_c::getSimpleTex
#include "d/d_bg_s_gnd_chk.h"    // dBgS_GndChk
#include "m_Do/m_Do_ext.h"       // mDoExt_modelEntryDL
#include "m_Do/m_Do_mtx.h"       // mDoMtx_stack_c, cMtx_copy, Mtx
#include "SSystem/SComponent/c_xyz.h"   // cXyz
#include "SSystem/SComponent/c_m3d.h"   // G_CM3D_F_INF
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphBase/J3DSys.h"

#include "dusk/online_puppet.h"
#include "dusk/online.h"
#include "dusk/frame_interpolation.h"
#include "dusk/logging.h"

#include <cstring>

namespace dusk::online::puppet {
namespace {

// Set while calc()'ing a remote-player puppet, which shares the local player's
// model data (and therefore its joint callbacks). The puppet has no daAlink_c
// behind its model, so the callbacks skip the player-specific joint control and
// instead inject the streamed pose below.
bool s_puppetCalc = false;

// Streamed world-space joint matrices (12 floats each) for the puppet currently
// being calc()'d, plus how many are valid. While set, the Link joint callbacks
// inject these in place of the local player's joint control, so envelope skinning,
// normal/bump matrices, and the PC frame-interpolation snapshot are all derived
// from the remote pose in a single calc() pass.
const f32* s_puppetJoints = NULL;
int        s_puppetJointCount = 0;

// Where each remote player's puppet was actually drawn this frame (its body base
// translation), keyed by player id, so the nameplate overlay can anchor exactly to
// the rendered puppet instead of re-deriving from the streamed transform.
float s_puppetWorldPos[kMaxPlayers][3] = {};
bool  s_puppetPosValid[kMaxPlayers] = {};

// Drive one puppet joint from the streamed pose during calc(). Runs at callback
// phase 0, where the joint's world matrix has just been computed and
// J3DSys::mCurrentMtx holds it; overriding both sets this joint absolutely and
// gives children the correct parent. Joints past the streamed count keep their
// computed (bind-relative) matrix as a fallback.
void apply_puppet_joint(J3DJoint* joint) {
    if (s_puppetJoints == NULL) return;
    int jntNo = joint->getJntNo();
    if (jntNo < 0 || jntNo >= s_puppetJointCount) return;
    Mtx m;
    memcpy(m, s_puppetJoints + jntNo * 12, sizeof(Mtx));
    j3dSys.getModel()->setAnmMtx(jntNo, m);
    cMtx_copy(m, J3DSys::mCurrentMtx);
}

}  // anonymous namespace

bool body_joint_hook(J3DJoint* joint, int phase) {
    if (!s_puppetCalc) return false;
    if (phase == 0) apply_puppet_joint(joint);
    return true;
}

bool head_joint_hook() {
    // Puppet head: the real headModelCallBack drives hair physics on the LOCAL
    // hat model using local actor state, which is wrong for a puppet (and would
    // corrupt the local player's hair). Leave the joint at its MtxCalc bind pose.
    return s_puppetCalc;
}

bool get_puppet_world_pos(int playerId, float out[3]) {
    if (playerId < 0 || playerId >= kMaxPlayers || !s_puppetPosValid[playerId]) return false;
    out[0] = s_puppetWorldPos[playerId][0];
    out[1] = s_puppetWorldPos[playerId][1];
    out[2] = s_puppetWorldPos[playerId][2];
    return true;
}

void update_and_draw(void* ctx, ModelCreateFn create,
                     J3DModel* bodyModel, J3DModel* hatModel,
                     J3DModel* handModel, J3DModel* faceModel, bool isWolf) {
    // Invalidate last frame's puppet anchors; each drawn puppet re-sets its own.
    for (int i = 0; i < kMaxPlayers; i++) s_puppetPosValid[i] = false;
    if (bodyModel == NULL) return;

    // --- publish our skeleton (model-space base TR + joint matrices) ---
    if (!isWolf) {
        static float s_baseTR[12];
        static float s_joints[kMaxJoints * 12];
        int jn = bodyModel->getModelData()->getJointNum();
        if (jn > kMaxJoints) jn = kMaxJoints;
        memcpy(s_baseTR, &bodyModel->getBaseTRMtx(), 12 * sizeof(f32));
        for (int j = 0; j < jn; j++) {
            memcpy(s_joints + j * 12, bodyModel->getAnmMtx(j), 12 * sizeof(f32));
        }
        set_local_pose(s_baseTR, s_joints, jn);
    }

    // A full human Link is several models: body (al.bmd), head+hair (al_head.bmd),
    // hands (al_hands.bmd) and face (al_face.bmd). The puppet needs all of them —
    // drawing only the body leaves it headless/handless. The sub-models attach to
    // the body's (already-synced) joints, so they follow the streamed pose; their
    // own internal joints (hair sway, fingers) render in a neutral bind pose.
    static J3DModel* s_puppetModels[kMaxPlayers] = {NULL};
    static J3DModel* s_puppetHeadModels[kMaxPlayers] = {NULL};
    static J3DModel* s_puppetHandModels[kMaxPlayers] = {NULL};
    static J3DModel* s_puppetFaceModels[kMaxPlayers] = {NULL};
    static u32 s_puppetShadowKeys[kMaxPlayers] = {0};
    static J3DModelData* s_puppetSrc = NULL;

    J3DModelData* src = bodyModel->getModelData();
    // Sub-model data comes from the local player's already-loaded models (NULL in
    // wolf form, where the body model contains everything).
    J3DModelData* headSrc = (hatModel != NULL) ? hatModel->getModelData() : NULL;
    J3DModelData* handSrc = (handModel != NULL) ? handModel->getModelData() : NULL;
    J3DModelData* faceSrc = (faceModel != NULL) ? faceModel->getModelData() : NULL;
    if (s_puppetSrc != src) {
        // Player model data changed (area/form change) — drop stale models.
        for (int i = 0; i < kMaxPlayers; i++) {
            s_puppetModels[i] = NULL;
            s_puppetHeadModels[i] = NULL;
            s_puppetHandModels[i] = NULL;
            s_puppetFaceModels[i] = NULL;
            s_puppetShadowKeys[i] = 0;
        }
        s_puppetSrc = src;
    }

    int n = remote_count();
    for (int i = 0; i < n && i < kMaxPlayers; i++) {
        const PlayerState* rp = remote_player(i);
        if (rp == NULL) continue;

        J3DModel*& model = s_puppetModels[i];
        if (model == NULL) {
            // Create through the actor's initModelEnv (mdlFlags=0) so the puppet
            // gets the SAME warp-material setup the real Link does (it detects the
            // WARP_TEX material on the head and adds diff-flag 0x2000400). Without
            // it the head renders as the raw black "digital dissolve" blocks.
            model = create(ctx, src, 0);
            if (model != NULL) {
                DuskLog.info("[online] puppet model created for '{}' (id {})", rp->name, rp->id);
            }
        }
        if (model == NULL) continue;

        // Create the matching head/hand/face sub-models (human form only).
        J3DModel*& headModel = s_puppetHeadModels[i];
        J3DModel*& handModel = s_puppetHandModels[i];
        J3DModel*& faceModelP = s_puppetFaceModels[i];
        if (headModel == NULL && headSrc != NULL) headModel = create(ctx, headSrc, 0);
        if (handModel == NULL && handSrc != NULL) handModel = create(ctx, handSrc, 0);
        if (faceModelP == NULL && faceSrc != NULL) faceModelP = create(ctx, faceSrc, 0x20200);

        model->setUserArea(reinterpret_cast<uintptr_t>(ctx));
        if (headModel != NULL) headModel->setUserArea(reinterpret_cast<uintptr_t>(ctx));
        if (handModel != NULL) handModel->setUserArea(reinterpret_cast<uintptr_t>(ctx));
        if (faceModelP != NULL) faceModelP->setUserArea(reinterpret_cast<uintptr_t>(ctx));

        // Apply the peer's streamed skeleton if available, else base pose.
        static float s_rbase[12];
        static float s_rjoints[kMaxJoints * 12];
        int rjn = 0;
        const bool havePose = get_remote_pose(rp->id, s_rbase, s_rjoints, &rjn);

        // One-time diagnostics: puppet skeleton coverage + frame-interp state.
        static bool s_loggedPuppetInfo = false;
        if (!s_loggedPuppetInfo && havePose) {
            s_loggedPuppetInfo = true;
            const int modelJoints = src->getJointNum();
            DuskLog.info("[online] puppet skeleton: model joints={}, weightEnvMtx={}, "
                         "streamed joints={} (cap {}), frameInterp={}",
                         modelJoints, src->getWEvlpMtxNum(), rjn, kMaxJoints,
                         dusk::frame_interp::is_enabled() ? "on" : "off");
            if (rjn < modelJoints) {
                DuskLog.warn("[online] puppet skeleton TRUNCATED: only {}/{} joints streamed; "
                             "joints {}+ fall back to bind pose (raise kMaxJoints)",
                             rjn, modelJoints, rjn);
            }
            for (int j = 0; j < rjn; j++) {
                const float* mtx = s_rjoints + j * 12;
                const float sumsq = mtx[0] * mtx[0] + mtx[4] * mtx[4] + mtx[8] * mtx[8];
                if (sumsq < 1.0e-8f) {
                    DuskLog.warn("[online] puppet joint {} streamed with a near-zero matrix "
                                 "(collapsed -> dissolving geometry)", j);
                }
            }
        }

        s_puppetCalc = true;
        if (havePose) {
            Mtx bt;
            memcpy(bt, s_rbase, 12 * sizeof(f32));
            // Feed the streamed pose through the joint callbacks during calc() so
            // envelopes, normal/bump matrices, and the interp snapshot are all
            // consistent with it (no post-calc overwrite).
            s_puppetJoints = s_rjoints;
            s_puppetJointCount = rjn;
            model->setBaseTRMtx(bt);
            model->calc();
            s_puppetJoints = NULL;
            s_puppetJointCount = 0;
        } else {
            mDoMtx_stack_c::transS(rp->pos[0], rp->pos[1], rp->pos[2]);
            mDoMtx_stack_c::YrotM(rp->angleY);
            model->setBaseTRMtx(mDoMtx_stack_c::get());
            model->calc();
        }

        // Record the puppet's drawn world position (body base translation) so the
        // nameplate overlay anchors exactly to the rendered puppet. Mtx is row-major
        // f32[3][4]; translation is elements [3],[7],[11].
        if (rp->id >= 0 && rp->id < kMaxPlayers) {
            const f32* base = reinterpret_cast<const f32*>(model->getBaseTRMtx());
            s_puppetWorldPos[rp->id][0] = base[3];
            s_puppetWorldPos[rp->id][1] = base[7];
            s_puppetWorldPos[rp->id][2] = base[11];
            s_puppetPosValid[rp->id] = true;
        }

        // Attach the sub-models to the now-posed body skeleton, exactly like the
        // real Link does: face + head hang off body joint 4 (head); hands off the
        // body base plus joints 9/0xE. s_puppetCalc stays set so the head callback
        // skips local hair physics (hand/face models have no joint callback).
        if (faceModelP != NULL) {
            faceModelP->setBaseTRMtx(model->getAnmMtx(4));
            faceModelP->calc();
        }
        if (headModel != NULL) {
            headModel->setBaseTRMtx(model->getAnmMtx(4));
            headModel->calc();
        }
        if (handModel != NULL) {
            handModel->setBaseTRMtx(model->getBaseTRMtx());
            handModel->calc();
            handModel->setAnmMtx(1, model->getAnmMtx(9));
            handModel->setAnmMtx(2, model->getAnmMtx(0xE));
        }
        s_puppetCalc = false;

        cXyz ppos(rp->pos[0], rp->pos[1], rp->pos[2]);
        // Shade the puppet IDENTICALLY to the real player: pick the env-light TEV
        // preset by form, then zero the custom additive TEV color registers (the
        // effect of daAlink_c::initTevCustomColor). A non-zero TevColor adds
        // ~half-white to every material — the washed-out/glowing look.
        dKy_tevstr_c tevstr;
        g_env_light.settingTevStruct(rp->isWolf ? 9 : 10, &ppos, &tevstr);
        tevstr.mLightInf.a = 0;
        tevstr.TevColor.r = 0;
        tevstr.TevColor.g = 0;
        tevstr.TevColor.b = 0;
        tevstr.TevKColor.r = 0;
        tevstr.TevKColor.b = 0;
        g_env_light.setLightTevColorType_MAJI(model, &tevstr);
        mDoExt_modelEntryDL(model);
        // Draw the sub-models with the same baked TEV/light state.
        if (faceModelP != NULL) {
            g_env_light.setLightTevColorType_MAJI(faceModelP, &tevstr);
            mDoExt_modelEntryDL(faceModelP);
        }
        if (headModel != NULL) {
            g_env_light.setLightTevColorType_MAJI(headModel, &tevstr);
            mDoExt_modelEntryDL(headModel);
        }
        if (handModel != NULL) {
            g_env_light.setLightTevColorType_MAJI(handModel, &tevstr);
            mDoExt_modelEntryDL(handModel);
        }

        // Cast a real (model-projected) shadow like the local Link. The puppet has
        // no collision, so raycast the ground under it. Use the puppet's ACTUAL drawn
        // position (the body base translation) — NOT rp->pos: the streamed transform
        // and the streamed pose baseTR use different Y origins, so raycasting from
        // rp->pos started the ray in the wrong place and blew up the height-above-
        // ground, failing realPolygonCheck. setShadow derives that height as
        // (param6 - param7); keep it ~a body height so there's a sane projection.
        const f32* pbase = reinterpret_cast<const f32*>(model->getBaseTRMtx());
        const f32 px = pbase[3], py = pbase[7], pz = pbase[11];
        cXyz shadowChkPos(px, py + 100.0f, pz);
        dBgS_GndChk gndChk;
        gndChk.SetPos(&shadowChkPos);
        f32 groundH = dComIfG_Bgsp().GroundCross(&gndChk);
        if (groundH != -G_CM3D_F_INF) {
            // Anchor the shadow at the puppet's FEET (which are on-screen), not at the
            // detected ground: realPolygonCheck frustum-clips its work box, and the
            // ground can sit well below the feet, so a ground-anchored box gets culled
            // (no shadow). Centered at the feet, the box is in view and ShdwDraw
            // projects the silhouette down onto the BG polys it finds below.
            cXyz shadowCenter(px, py, pz);
            u32& shadowKey = s_puppetShadowKeys[i];
            const f32 bodyRefY = py + 130.0f;  // ~Link height; (param6-param7)=height
            shadowKey = dComIfGd_setShadow(shadowKey, 0, model, &shadowCenter,
                                           800.0f, 0.0f, bodyRefY, py, gndChk,
                                           &tevstr, 0, 1.0f,
                                           dDlst_shadowControl_c::getSimpleTex());
            if (shadowKey != 0) {
                if (faceModelP != NULL) dComIfGd_addRealShadow(shadowKey, faceModelP);
                if (headModel != NULL) dComIfGd_addRealShadow(shadowKey, headModel);
                if (handModel != NULL) dComIfGd_addRealShadow(shadowKey, handModel);
            }
        }
    }
}

}  // namespace dusk::online::puppet
