#ifndef DUSK_ONLINE_PUPPET_H
#define DUSK_ONLINE_PUPPET_H

// Remote-player puppet rendering, extracted from the decompiled d_a_alink.cpp so
// the actor only carries thin hook calls. Renders each remote player as a full
// Link (body + head/hair + hands + face) driven by the streamed skeletal pose,
// shaded like the local player, with a projected shadow.

class J3DModel;
class J3DModelData;
class J3DJoint;

namespace dusk::online::puppet {

// Creates a warp-material-aware Link model from shared model data. Supplied by the
// actor because creation needs daAlink_c::initModelEnv; `ctx` is the local
// daAlink_c passed straight back. Using a thunk keeps this module decoupled from
// the decompiled actor type.
using ModelCreateFn = J3DModel* (*)(void* ctx, J3DModelData* data, unsigned int diffFlags);

// Joint-callback hooks. Call at the top of the decompiled Link joint callbacks;
// returns true when the puppet pass handled the joint (the caller then returns 1).
// body_joint_hook serves both the body and the wolf callback.
bool body_joint_hook(J3DJoint* joint, int phase);
bool head_joint_hook();

// Called once per frame from daAlink_c::draw(): publishes the local skeleton for
// peers and renders every remote player's puppet. ctx/create/bodyModel are the
// local player's; hatModel/handModel/faceModel are NULL in wolf form.
void update_and_draw(void* ctx, ModelCreateFn create,
                     J3DModel* bodyModel, J3DModel* hatModel,
                     J3DModel* handModel, J3DModel* faceModel, bool isWolf);

}  // namespace dusk::online::puppet

#endif  // DUSK_ONLINE_PUPPET_H
