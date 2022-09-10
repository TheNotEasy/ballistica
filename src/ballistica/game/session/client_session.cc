// Released under the MIT License. See LICENSE for details.

#include "ballistica/game/session/client_session.h"

#include "ballistica/app/app.h"
#include "ballistica/assets/component/collide_model.h"
#include "ballistica/assets/component/model.h"
#include "ballistica/assets/component/sound.h"
#include "ballistica/assets/component/texture.h"
#include "ballistica/audio/audio.h"
#include "ballistica/dynamics/bg/bg_dynamics.h"
#include "ballistica/dynamics/material/material.h"
#include "ballistica/dynamics/material/material_action.h"
#include "ballistica/dynamics/material/material_component.h"
#include "ballistica/dynamics/material/material_condition_node.h"
#include "ballistica/dynamics/rigid_body.h"
#include "ballistica/graphics/graphics.h"
#include "ballistica/networking/networking.h"
#include "ballistica/python/python.h"
#include "ballistica/scene/node/node_attribute.h"
#include "ballistica/scene/node/node_type.h"
#include "ballistica/scene/scene.h"
#include "ballistica/scene/scene_stream.h"

namespace ballistica {

ClientSession::ClientSession() { ClearSessionObjs(); }

void ClientSession::Reset(bool rewind) {
  assert(!shutting_down_);
  OnReset(rewind);
}

void ClientSession::OnReset(bool rewind) {
  ClearSessionObjs();
  target_base_time_ = 0.0;
  base_time_ = 0;
}

void ClientSession::ClearSessionObjs() {
  scenes_.clear();
  nodes_.clear();
  textures_.clear();
  models_.clear();
  sounds_.clear();
  collide_models_.clear();
  materials_.clear();
  commands_pending_.clear();
  commands_.clear();
  base_time_buffered_ = 0;
}

auto ClientSession::DoesFillScreen() const -> bool {
  // Look for any scene that has something that covers the background.
  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (const auto& scene : scenes_) {
    if ((scene.exists()) && (*scene).has_bg_cover()) {
      return true;
    }
  }
  return false;
}

void ClientSession::Draw(FrameDef* f) {
  // Just go through and draw all of our scenes.
  for (auto&& i : scenes_) {
    // NOTE - here we draw scenes in the order they were created, but
    // in a host-session we draw session first followed by activities
    // (that should be the same order in both cases, but just something to keep
    // in mind...)
    if (i.exists()) {
      i->Draw(f);
    }
  }
}

auto ClientSession::ReadByte() -> uint8_t {
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - 1) {
    throw Exception("state read error");
  }
  return *(current_cmd_ptr_++);
}

auto ClientSession::ReadInt32() -> int32_t {
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - 4) {
    throw Exception("state read error");
  }
  int32_t val;
  memcpy(&val, current_cmd_ptr_, sizeof(val));
  current_cmd_ptr_ += 4;
  return val;
}

auto ClientSession::ReadFloat() -> float {
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - 4) {
    throw Exception("state read error");
  }
  float val;
  memcpy(&val, current_cmd_ptr_, 4);
  current_cmd_ptr_ += 4;
  return val;
}

void ClientSession::ReadFloats(int count, float* vals) {
  int size = 4 * count;
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - size) {
    throw Exception("state read error");
  }
  memcpy(vals, current_cmd_ptr_, static_cast<size_t>(size));
  current_cmd_ptr_ += size;
}

void ClientSession::ReadInt32s(int count, int32_t* vals) {
  int size = 4 * count;
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - size) {
    throw Exception("state read error");
  }
  memcpy(vals, current_cmd_ptr_, static_cast<size_t>(size));
  current_cmd_ptr_ += size;
}

void ClientSession::ReadChars(int count, char* vals) {
  int size = count;
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - size) {
    throw Exception("state read error");
  }
  memcpy(vals, current_cmd_ptr_, static_cast<size_t>(size));
  current_cmd_ptr_ += size;
}

void ClientSession::ReadInt32_3(int32_t* vals) {
  size_t size = 3 * 4;
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - size) {
    throw Exception("state read error");
  }
  memcpy(vals, current_cmd_ptr_, size);
  current_cmd_ptr_ += size;
}

void ClientSession::ReadInt32_4(int32_t* vals) {
  size_t size = 4 * 4;
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - size) {
    throw Exception("state read error");
  }
  memcpy(vals, current_cmd_ptr_, size);
  current_cmd_ptr_ += size;
}

void ClientSession::ReadInt32_2(int32_t* vals) {
  size_t size = 2 * 4;
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - size) {
    throw Exception("state read error");
  }
  memcpy(vals, current_cmd_ptr_, size);
  current_cmd_ptr_ += size;
}

auto ClientSession::ReadString() -> std::string {
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - 4) {
    throw Exception("state read error");
  }
  int32_t size;
  memcpy(&size, current_cmd_ptr_, sizeof(size));
  current_cmd_ptr_ += 4;
  std::vector<char> buffer(static_cast<size_t>(size + 1));
  if (current_cmd_ptr_ > &(current_cmd_[0]) + current_cmd_.size() - size) {
    throw Exception("state read error");
  }
  memcpy(&(buffer[0]), current_cmd_ptr_, static_cast<size_t>(size));
  current_cmd_ptr_ += size;
  return &(buffer[0]);
}

void ClientSession::Update(int time_advance) {
  if (shutting_down_) {
    return;
  }

  // Allow replays to modulate speed, etc.
  // QUESTION: can we just use consume_rate_ for this?
  time_advance = GetActualTimeAdvance(time_advance);

  target_base_time_ += static_cast<double>(time_advance) * consume_rate_;

  try {
    // Read and run all events up to our target time.
    while (base_time_ < target_base_time_) {
      // If we need to do something explicit to keep messages flowing in.
      // (informing the replay thread to feed us more, etc.).
      FetchMessages();

      // If we've got another command on the list, pull it and run it.
      if (!commands_.empty()) {
        // Debugging: if this was previously pointed at a buffer, make sure we
        // went exactly to the end.
        if (g_buildconfig.debug_build()) {
          if (current_cmd_ptr_ != nullptr) {
            if (current_cmd_ptr_ != &(current_cmd_[0]) + current_cmd_.size()) {
              Log("SIZE ERROR FOR CMD "
                  + std::to_string(static_cast<int>(current_cmd_[0]))
                  + " expected " + std::to_string(current_cmd_.size()) + " got "
                  + std::to_string(current_cmd_ptr_ - &(current_cmd_[0])));
            }
          }
          assert(current_cmd_ptr_ == current_cmd_.data() + current_cmd_.size());
        }
        current_cmd_ = commands_.front();
        commands_.pop_front();
        current_cmd_ptr_ = &(current_cmd_[0]);
      } else {
        // Let the subclass know this happened. Replays may want to pause
        // playback until more data comes in but things like net-play may want
        // to just soldier on and skip ahead once data comes in.
        OnCommandBufferUnderrun();
        return;
      }

      auto cmd = static_cast<SessionCommand>(ReadByte());

      switch (cmd) {
        case SessionCommand::kBaseTimeStep: {
          int32_t stepsize = ReadInt32();
          BA_PRECONDITION(stepsize > 0);
          if (stepsize > 10000) {
            throw Exception(
                "got abnormally large stepsize; probably a corrupt stream");
          }
          base_time_buffered_ -= stepsize;
          BA_PRECONDITION(base_time_buffered_ >= 0);
          base_time_ += stepsize;
          break;
        }
        case SessionCommand::kDynamicsCorrection: {
          bool blend = current_cmd_[1];
          uint32_t offset = 2;
          uint16_t node_count;
          memcpy(&node_count, current_cmd_.data() + offset, sizeof(node_count));
          offset += 2;
          for (int i = 0; i < node_count; i++) {
            uint32_t node_id;
            memcpy(&node_id, current_cmd_.data() + offset, sizeof(node_id));
            offset += 4;
            int body_count = current_cmd_[offset++];
            Node* n =
                (node_id < nodes_.size()) ? nodes_[node_id].get() : nullptr;
            for (int j = 0; j < body_count; j++) {
              int bodyid = current_cmd_[offset++];
              uint16_t body_data_len;
              memcpy(&body_data_len, current_cmd_.data() + offset,
                     sizeof(body_data_len));
              RigidBody* b = n ? n->GetRigidBody(bodyid) : nullptr;
              offset += 2;
              const char* p1 = reinterpret_cast<char*>(&(current_cmd_[offset]));
              const char* p2 = p1;
              if (b) {
                dBodyID body = b->body();
                const dReal* p = dBodyGetPosition(body);
                float old_x = p[0];
                float old_y = p[1];
                float old_z = p[2];
                b->ExtractFull(&p2);
                if (p2 - p1 != body_data_len)
                  throw Exception("Invalid rbd correction data");
                if (blend) {
                  b->AddBlendOffset(old_x - p[0], old_y - p[1], old_z - p[2]);
                }
              }
              offset += body_data_len;
              if (offset > current_cmd_.size()) {
                throw Exception("Invalid rbd correction data");
              }
            }
            if (offset > current_cmd_.size())
              throw Exception("Invalid rbd correction data");

            // Extract custom per-node data.
            uint16_t custom_data_len;
            memcpy(&custom_data_len, current_cmd_.data() + offset,
                   sizeof(custom_data_len));
            offset += 2;
            if (custom_data_len != 0) {
              std::vector<uint8_t> data(custom_data_len);
              memcpy(&(data[0]), &(current_cmd_[offset]), custom_data_len);
              if (n) n->ApplyResyncData(data);
              offset += custom_data_len;
            }
            if (offset > current_cmd_.size()) {
              throw Exception("Invalid rbd correction data");
            }
          }
          if (offset != current_cmd_.size()) {
            throw Exception("invalid rbd correction data");
          }
          current_cmd_ptr_ = &(current_cmd_[0]) + offset;

          break;
        }
        case SessionCommand::kEndOfFile: {
          // EOF can happen anytime if they run out of disk space/etc.
          // We should expect any state.
          Reset(true);
          break;
        }
        case SessionCommand::kAddSceneGraph: {
          int32_t cmdvals[2];
          ReadInt32_2(cmdvals);
          int32_t id = cmdvals[0];
          millisecs_t starttime = cmdvals[1];
          if (id < 0 || id > 100) {
            throw Exception("invalid scene id");
          }
          if (static_cast<int>(scenes_.size()) < (id + 1)) {
            scenes_.resize(static_cast<size_t>(id) + 1);
          }
          assert(!scenes_[id].exists());
          scenes_[id] = Object::New<Scene>(starttime);
          scenes_[id]->set_stream_id(id);
          break;
        }
        case SessionCommand::kRemoveSceneGraph: {
          int32_t id = ReadInt32();
          GetScene(id);  // Make sure it's valid.
          scenes_[id].Clear();
          break;
        }
        case SessionCommand::kStepSceneGraph: {
          int32_t val = ReadInt32();
          Scene* sg = GetScene(val);
          sg->Step();
          break;
        }
        case SessionCommand::kAddNode: {
          int32_t vals[3];  // scene-id, nodetype-id, node-id
          ReadInt32_3(vals);
          Scene* scene = GetScene(vals[0]);
          assert(g_app != nullptr);
          if (vals[1] < 0
              || vals[1] >= static_cast<int>(g_app->node_types_by_id.size())) {
            throw Exception("invalid node type id");
          }

          NodeType* node_type = g_app->node_types_by_id[vals[1]];

          // Fail if we get a ridiculous number of nodes.
          // FIXME: should enforce this on the server side too.
          int id = vals[2];
          if (id < 0 || id > 10000) {
            throw Exception("invalid node id");
          }
          if (static_cast<int>(nodes_.size()) < (id + 1)) {
            nodes_.resize(static_cast<size_t>(id) + 1);
          }
          assert(!nodes_[id].exists());
          {
            ScopedSetContext _cp(this);
            nodes_[id] = scene->NewNode(node_type->name(), "", nullptr);
            nodes_[id]->set_stream_id(id);
          }
          break;
        }
        case SessionCommand::kSetForegroundSceneGraph: {
          Scene* scene = GetScene(ReadInt32());
          g_game->SetForegroundScene(scene);
          break;
        }
        case SessionCommand::kNodeMessage: {
          int32_t vals[2];
          ReadInt32_2(vals);
          Node* n = GetNode(vals[0]);
          int32_t msg_size = vals[1];
          if (msg_size < 1 || msg_size > 10000) {
            throw Exception("invalid message");
          }
          std::vector<char> buffer(static_cast<size_t>(msg_size));
          ReadChars(msg_size, &buffer[0]);
          n->DispatchNodeMessage(&buffer[0]);
          break;
        }
        case SessionCommand::kConnectNodeAttribute: {
          int32_t vals[4];
          ReadInt32_4(vals);
          Node* src_node = GetNode(vals[0]);
          Node* dst_node = GetNode(vals[2]);
          NodeAttributeUnbound* src_attr =
              src_node->type()->GetAttribute(static_cast<uint32_t>(vals[1]));
          NodeAttributeUnbound* dst_attr =
              dst_node->type()->GetAttribute(static_cast<uint32_t>(vals[3]));
          src_node->ConnectAttribute(src_attr, dst_node, dst_attr);
          break;
        }
        case SessionCommand::kNodeOnCreate: {
          Node* n = GetNode(ReadInt32());
          n->OnCreate();
          break;
        }
        case SessionCommand::kAddMaterial: {
          int32_t vals[2];  // scene-id, material-id
          ReadInt32_2(vals);
          Scene* scene = GetScene(vals[0]);
          // Fail if we get a ridiculous number of materials.
          // FIXME: should enforce this on the server side too.
          int id = vals[1];
          if (vals[1] < 0 || vals[1] >= 1000) {
            throw Exception("invalid material id");
          }
          if (static_cast<int>(materials_.size()) < (id + 1)) {
            materials_.resize(static_cast<size_t>(id) + 1);
          }
          assert(!materials_[id].exists());
          materials_[id] = Object::New<Material>("", scene);
          materials_[id]->stream_id_ = id;
          break;
        }
        case SessionCommand::kRemoveMaterial: {
          int id = ReadInt32();
          GetMaterial(id);  // make sure its valid
          materials_[id].Clear();
          break;
        }
        case SessionCommand::kAddMaterialComponent: {
          int32_t cmdvals[2];
          ReadInt32_2(cmdvals);
          Material* m = GetMaterial(cmdvals[0]);
          int component_size = cmdvals[1];
          if (component_size < 1 || component_size > 10000) {
            throw Exception("invalid component");
          }
          std::vector<char> buffer(static_cast<size_t>(component_size));
          ReadChars(component_size, &buffer[0]);
          auto c(Object::New<MaterialComponent>());
          const char* ptr1 = &buffer[0];
          const char* ptr2 = ptr1;
          c->Restore(&ptr2, this);
          BA_PRECONDITION(ptr2 - ptr1 == component_size);
          m->AddComponent(c);
          break;
        }
        case SessionCommand::kAddTexture: {
          int32_t vals[2];  // scene-id, texture-id
          ReadInt32_2(vals);
          std::string name = ReadString();
          Scene* scene = GetScene(vals[0]);
          // Fail if we get a ridiculous number of textures.
          // FIXME: Should enforce this on the server side too.
          int id = vals[1];
          if (vals[1] < 0 || vals[1] >= 1000) {
            throw Exception("invalid texture id");
          }
          if (static_cast<int>(textures_.size()) < (id + 1)) {
            textures_.resize(static_cast<size_t>(id) + 1);
          }
          assert(!textures_[id].exists());
          textures_[id] = Object::New<Texture>(name, scene);
          textures_[id]->stream_id_ = id;
          break;
        }
        case SessionCommand::kRemoveTexture: {
          int id = ReadInt32();
          GetTexture(id);  // make sure its valid
          textures_[id].Clear();
          break;
        }
        case SessionCommand::kAddModel: {
          int32_t vals[2];  // scene-id, model-id
          ReadInt32_2(vals);
          std::string name = ReadString();
          Scene* scene = GetScene(vals[0]);

          // Fail if we get a ridiculous number of models.
          // FIXME: Should enforce this on the server side too.
          int id = vals[1];
          if (vals[1] < 0 || vals[1] >= 1000) {
            throw Exception("invalid model id");
          }
          if (static_cast<int>(models_.size()) < (id + 1)) {
            models_.resize(static_cast<size_t>(id) + 1);
          }
          assert(!models_[id].exists());
          models_[id] = Object::New<Model>(name, scene);
          models_[id]->stream_id_ = id;
          break;
        }
        case SessionCommand::kRemoveModel: {
          int id = ReadInt32();
          GetModel(id);  // make sure its valid
          models_[id].Clear();
          break;
        }
        case SessionCommand::kAddSound: {
          int32_t vals[2];  // scene-id, sound-id
          ReadInt32_2(vals);
          std::string name = ReadString();
          Scene* scene = GetScene(vals[0]);
          // Fail if we get a ridiculous number of sounds.
          // FIXME: Should enforce this on the server side too.
          int id = vals[1];
          if (vals[1] < 0 || vals[1] >= 1000) {
            throw Exception("invalid sound id");
          }
          if (static_cast<int>(sounds_.size()) < (id + 1)) {
            sounds_.resize(static_cast<size_t>(id) + 1);
          }
          assert(!sounds_[id].exists());
          sounds_[id] = Object::New<Sound>(name, scene);
          sounds_[id]->stream_id_ = id;
          break;
        }
        case SessionCommand::kRemoveSound: {
          int id = ReadInt32();
          GetSound(id);  // Make sure its valid.
          sounds_[id].Clear();
          break;
        }
        case SessionCommand::kAddCollideModel: {
          int32_t vals[2];  // scene-id, collide_model-id
          ReadInt32_2(vals);
          std::string name = ReadString();
          Scene* scene = GetScene(vals[0]);

          // Fail if we get a ridiculous number of collide_models.
          // FIXME: Should enforce this on the server side too.
          int id = vals[1];
          if (vals[1] < 0 || vals[1] >= 1000) {
            throw Exception("invalid collide_model id");
          }
          if (static_cast<int>(collide_models_.size()) < (id + 1)) {
            collide_models_.resize(static_cast<size_t>(id) + 1);
          }
          assert(!collide_models_[id].exists());
          collide_models_[id] = Object::New<CollideModel>(name, scene);
          collide_models_[id]->stream_id_ = id;
          break;
        }
        case SessionCommand::kRemoveCollideModel: {
          int id = ReadInt32();
          GetCollideModel(id);  // make sure its valid
          collide_models_[id].Clear();
          break;
        }
        case SessionCommand::kRemoveNode: {
          int id = ReadInt32();
          Node* n = GetNode(id);
          n->scene()->DeleteNode(n);
          assert(!nodes_[id].exists());
          break;
        }
        case SessionCommand::kSetNodeAttrFloat: {
          int vals[2];
          ReadInt32_2(vals);
          GetNode(vals[0])->GetAttribute(vals[1]).Set(ReadFloat());
          break;
        }
        case SessionCommand::kSetNodeAttrInt32: {
          int32_t vals[3];
          ReadInt32_3(vals);

          // Note; we currently deal in 64 bit ints locally but read/write 32
          // bit over the wire.
          GetNode(vals[0])->GetAttribute(vals[1]).Set(
              static_cast<int64_t>(vals[2]));
          break;
        }
        case SessionCommand::kSetNodeAttrBool: {
          int vals[3];
          ReadInt32_3(vals);
          GetNode(vals[0])->GetAttribute(vals[1]).Set(
              static_cast<bool>(vals[2]));
          break;
        }
        case SessionCommand::kSetNodeAttrFloats: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<float> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadFloats(count, &(vals[0]));
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals);
          break;
        }
        case SessionCommand::kSetNodeAttrInt32s: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<int32_t> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadInt32s(count, &(vals[0]));
          }

          // Note: we currently deal in 64 bit ints locally but read/write 32
          // bit over the wire. Convert.
          std::vector<int64_t> vals64(static_cast<size_t>(count));
          for (int i = 0; i < count; i++) {
            vals64[i] = vals[i];
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals64);
          break;
        }
        case SessionCommand::kSetNodeAttrString: {
          int vals[2];
          ReadInt32_2(vals);
          GetNode(vals[0])->GetAttribute(vals[1]).Set(ReadString());
          break;
        }
        case SessionCommand::kSetNodeAttrNode: {
          int vals[3];
          ReadInt32_3(vals);
          GetNode(vals[0])->GetAttribute(vals[1]).Set(GetNode(vals[2]));
          break;
        }
        case SessionCommand::kSetNodeAttrNodeNull: {
          int cmdvals[2];
          ReadInt32_2(cmdvals);
          Node* val = nullptr;
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrTextureNull: {
          int cmdvals[2];
          ReadInt32_2(cmdvals);
          Texture* val = nullptr;
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrSoundNull: {
          int cmdvals[2];
          ReadInt32_2(cmdvals);
          Sound* val = nullptr;
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrModelNull: {
          int cmdvals[2];
          ReadInt32_2(cmdvals);
          Model* val = nullptr;
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrCollideModelNull: {
          int cmdvals[2];
          ReadInt32_2(cmdvals);
          CollideModel* val = nullptr;
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrNodes: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<int32_t> vals_in(static_cast<size_t>(count));
          std::vector<Node*> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadInt32s(count, &(vals_in[0]));
          }
          for (int i = 0; i < count; i++) {
            vals[i] = GetNode(vals_in[i]);
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals);
          break;
        }
        case SessionCommand::kSetNodeAttrTexture: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          Texture* val = GetTexture(cmdvals[2]);
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrTextures: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<int32_t> vals_in(static_cast<size_t>(count));
          std::vector<Texture*> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadInt32s(count, &(vals_in[0]));
          }
          for (int i = 0; i < count; i++) {
            vals[i] = GetTexture(vals_in[i]);
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals);
          break;
        }
        case SessionCommand::kSetNodeAttrSound: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          Sound* val = GetSound(cmdvals[2]);
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrSounds: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<int32_t> vals_in(static_cast<size_t>(count));
          std::vector<Sound*> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadInt32s(count, &(vals_in[0]));
          }
          for (int i = 0; i < count; i++) {
            vals[i] = GetSound(vals_in[i]);
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals);
          break;
        }
        case SessionCommand::kSetNodeAttrModel: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          Model* val = GetModel(cmdvals[2]);
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrModels: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<int32_t> vals_in(static_cast<size_t>(count));
          std::vector<Model*> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadInt32s(count, &(vals_in[0]));
          }
          for (int i = 0; i < count; i++) {
            vals[i] = GetModel(vals_in[i]);
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals);
          break;
        }
        case SessionCommand::kSetNodeAttrCollideModel: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          CollideModel* val = GetCollideModel(cmdvals[2]);
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(val);
          break;
        }
        case SessionCommand::kSetNodeAttrCollideModels: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<int32_t> vals_in(static_cast<size_t>(count));
          std::vector<CollideModel*> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadInt32s(count, &(vals_in[0]));
          }
          for (int i = 0; i < count; i++) {
            vals[i] = GetCollideModel(vals_in[i]);
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals);
          break;
        }
        case SessionCommand::kSetNodeAttrMaterials: {
          int cmdvals[3];
          ReadInt32_3(cmdvals);
          int count = cmdvals[2];
          if (count < 0 || count > 1000) {
            throw Exception("invalid array size (" + std::to_string(count)
                            + ")");
          }
          std::vector<int32_t> vals_in(static_cast<size_t>(count));
          std::vector<Material*> vals(static_cast<size_t>(count));
          if (count > 0) {
            ReadInt32s(count, &(vals_in[0]));
          }
          for (int i = 0; i < count; i++) {
            vals[i] = GetMaterial(vals_in[i]);
          }
          GetNode(cmdvals[0])->GetAttribute(cmdvals[1]).Set(vals);
          break;
        }
        case SessionCommand::kPlaySound: {
          Sound* sound = GetSound(ReadInt32());
          float volume = ReadFloat();
          g_audio->PlaySound(sound->GetSoundData(), volume);
          break;
        }
        case SessionCommand::kScreenMessageBottom: {
          std::string val = ReadString();
          Vector3f color{};
          ReadFloats(3, color.v);
          ScreenMessage(val, color);
          break;
        }
        case SessionCommand::kScreenMessageTop: {
          int cmdvals[2];
          ReadInt32_2(cmdvals);
          Texture* texture = GetTexture(cmdvals[0]);
          Texture* tint_texture = GetTexture(cmdvals[1]);
          std::string s = ReadString();
          float f[9];
          ReadFloats(9, f);
          g_graphics->AddScreenMessage(
              s, Vector3f(f[0], f[1], f[2]), true, texture, tint_texture,
              Vector3f(f[3], f[4], f[5]), Vector3f(f[6], f[7], f[8]));
          break;
        }
        case SessionCommand::kPlaySoundAtPosition: {
          Sound* sound = GetSound(ReadInt32());
          float volume = ReadFloat();
          float x = ReadFloat();
          float y = ReadFloat();
          float z = ReadFloat();
          g_audio->PlaySoundAtPosition(sound->GetSoundData(), volume, x, y, z);
          break;
        }
        case SessionCommand::kEmitBGDynamics: {
          int cmdvals[4];
          ReadInt32_4(cmdvals);
          float vals[8];
          ReadFloats(8, vals);
          if (g_bg_dynamics != nullptr) {
            BGDynamicsEmission e;
            e.emit_type = (BGDynamicsEmitType)cmdvals[0];
            e.count = cmdvals[1];
            e.chunk_type = (BGDynamicsChunkType)cmdvals[2];
            e.tendril_type = (BGDynamicsTendrilType)cmdvals[3];
            e.position.x = vals[0];
            e.position.y = vals[1];
            e.position.z = vals[2];
            e.velocity.x = vals[3];
            e.velocity.y = vals[4];
            e.velocity.z = vals[5];
            e.scale = vals[6];
            e.spread = vals[7];
            g_bg_dynamics->Emit(e);
          }
          break;
        }
        default:
          throw Exception("unrecognized stream command: "
                          + std::to_string(static_cast<int>(cmd)));
      }
    }
  } catch (const std::exception& e) {
    Error(e.what());
  }
}  // NOLINT  (yes this is too long)

ClientSession::~ClientSession() = default;

void ClientSession::ScreenSizeChanged() {
  // Let all our scenes know.
  for (auto&& i : scenes_) {
    if (Scene* sg = i.get()) {
      sg->ScreenSizeChanged();
    }
  }
}

void ClientSession::LanguageChanged() {
  // Let all our scenes know.
  for (auto&& i : scenes_) {
    if (Scene* sg = i.get()) {
      sg->LanguageChanged();
    }
  }
}

auto ClientSession::GetScene(int id) const -> Scene* {
  if (id < 0 || id >= static_cast<int>(scenes_.size())) {
    throw Exception("Invalid scene id");
  }
  Scene* sg = scenes_[id].get();
  if (!sg) {
    throw Exception("Invalid scene id");
  }
  return sg;
}
auto ClientSession::GetNode(int id) const -> Node* {
  if (id < 0 || id >= static_cast<int>(nodes_.size())) {
    throw Exception("Invalid node (out of range)");
  }
  Node* n = nodes_[id].get();
  if (!n) {
    throw Exception("Invalid node id (empty slot)");
  }
  return n;
}
auto ClientSession::GetMaterial(int id) const -> Material* {
  if (id < 0 || id >= static_cast<int>(materials_.size())) {
    throw Exception("Invalid material (out of range)");
  }
  Material* n = materials_[id].get();
  if (!n) {
    throw Exception("Invalid material id (empty slot)");
  }
  return n;
}
auto ClientSession::GetTexture(int id) const -> Texture* {
  if (id < 0 || id >= static_cast<int>(textures_.size())) {
    throw Exception("Invalid texture (out of range)");
  }
  Texture* n = textures_[id].get();
  if (!n) {
    throw Exception("Invalid texture id (empty slot)");
  }
  return n;
}
auto ClientSession::GetModel(int id) const -> Model* {
  if (id < 0 || id >= static_cast<int>(models_.size())) {
    throw Exception("Invalid model (out of range)");
  }
  Model* n = models_[id].get();
  if (!n) {
    throw Exception("Invalid model id (empty slot)");
  }
  return n;
}
auto ClientSession::GetSound(int id) const -> Sound* {
  if (id < 0 || id >= static_cast<int>(sounds_.size())) {
    throw Exception("Invalid sound (out of range)");
  }
  Sound* n = sounds_[id].get();
  if (!n) {
    throw Exception("Invalid sound id (empty slot)");
  }
  return n;
}
auto ClientSession::GetCollideModel(int id) const -> CollideModel* {
  if (id < 0 || id >= static_cast<int>(collide_models_.size())) {
    throw Exception("Invalid collide_model (out of range)");
  }
  CollideModel* n = collide_models_[id].get();
  if (!n) {
    throw Exception("Invalid collide_model id (empty slot)");
  }
  return n;
}

void ClientSession::Error(const std::string& description) {
  Log("ERROR: client session error: " + description);
  End();
}

void ClientSession::End() {
  if (shutting_down_) return;
  shutting_down_ = true;
  g_python->PushObjCall(Python::ObjID::kLaunchMainMenuSessionCall);
}

void ClientSession::HandleSessionMessage(const std::vector<uint8_t>& buffer) {
  assert(InLogicThread());

  BA_PRECONDITION(!buffer.empty());

  switch (buffer[0]) {
    case BA_MESSAGE_SESSION_RESET: {
      // Hmmm; been a while since I wrote this, but wondering why reset isn't
      // just a session-command. (Do we not want it added to replay streams?...)
      Reset(false);
      break;
    }

    case BA_MESSAGE_SESSION_COMMANDS: {
      // This is simply 16 bit length followed by command up to the end of the
      // packet. Break it apart and feed each command to the client session.
      uint32_t offset = 1;
      std::vector<uint8_t> sub_buffer;
      while (true) {
        uint16_t size;
        memcpy(&size, &(buffer[offset]), 2);
        if (offset + size > buffer.size()) {
          Error("invalid state message");
          return;
        }
        sub_buffer.resize(size);
        memcpy(&(sub_buffer[0]), &(buffer[offset + 2]), sub_buffer.size());
        AddCommand(sub_buffer);
        offset += 2 + size;  // move to next command
        if (offset == buffer.size()) {
          // let's also use this opportunity to graph our command-buffer size
          // for network debugging... if (NetGraph *graph =
          // g_graphics->GetClientSessionStepBufferGraph()) {
          //   graph->addSample(GetRealTime(), steps_on_list_);
          // }

          break;
        }
      }
      break;
    }

    case BA_MESSAGE_SESSION_DYNAMICS_CORRECTION: {
      // Just drop this in the game's command-stream verbatim, except switch its
      // state-ID to a command-ID.
      std::vector<uint8_t> buffer_out = buffer;
      buffer_out[0] = static_cast<uint8_t>(SessionCommand::kDynamicsCorrection);
      AddCommand(buffer_out);
      break;
    }

    default:
      throw Exception("ClientSession::HandleSessionMessage " + ObjToString(this)
                      + "got unrecognized message : "
                      + std::to_string(static_cast<int>(buffer[0]))
                      + " of size " + std::to_string(buffer.size()));
      break;
  }
}

// Add a single command in.
void ClientSession::AddCommand(const std::vector<uint8_t>& command) {
  // If this is a time-step command, we can dump everything we've been building
  // up onto the list to be chewed through by the interpreter (we don't want to
  // add things until we have the *entire* step, so we don't wind up rendering
  // things halfway through some change, etc.).
  commands_pending_.push_back(command);
  if (!command.empty()) {
    if (command[0] == static_cast<uint8_t>(SessionCommand::kBaseTimeStep)) {
      // Keep a tally of how much stepped time we've built up.
      base_time_buffered_ += command[1];

      // Let subclasses know we just received a step in case they'd like
      // to factor it in for rate adjustments/etc.
      OnBaseTimeStepAdded(command[1]);

      for (auto&& i : commands_pending_) {
        commands_.push_back(i);
      }
      commands_pending_.clear();
    }
  }
}

auto ClientSession::GetForegroundContext() -> Context { return Context(this); }

void ClientSession::GetCorrectionMessages(
    bool blend, std::vector<std::vector<uint8_t> >* messages) {
  std::vector<uint8_t> message;
  for (auto&& i : scenes_) {
    if (Scene* sg = i.get()) {
      message = sg->GetCorrectionMessage(blend);
      // A correction packet of size 4 is empty; ignore it.
      if (message.size() > 4) {
        messages->push_back(message);
      }
    }
  }
}

void ClientSession::DumpFullState(SceneStream* out) {
  // Add all scenes.
  for (auto&& i : scenes()) {
    if (Scene* sg = i.get()) {
      sg->Dump(out);
    }
  }

  // Before doing any nodes, we need to create all materials.
  // (but *not* their components, which may reference the nodes that we haven't
  // made yet)
  for (auto&& i : materials()) {
    if (Material* m = i.get()) {
      out->AddMaterial(m);
    }
  }

  // Add all media.
  for (auto&& i : textures()) {
    if (Texture* t = i.get()) {
      out->AddTexture(t);
    }
  }
  for (auto&& i : models()) {
    if (Model* s = i.get()) {
      out->AddModel(s);
    }
  }
  for (auto&& i : sounds()) {
    if (Sound* s = i.get()) {
      out->AddSound(s);
    }
  }
  for (auto&& i : collide_models()) {
    if (CollideModel* s = i.get()) {
      out->AddCollideModel(s);
    }
  }

  // Add all scene nodes.
  for (auto&& i : scenes()) {
    if (Scene* sg = i.get()) {
      sg->DumpNodes(out);
    }
  }

  // Now fill out materials since we know all the nodes/etc. that they
  // refer to exist.
  for (auto&& i : materials()) {
    if (Material* m = i.get()) {
      m->DumpComponents(out);
    }
  }
}

}  // namespace ballistica
