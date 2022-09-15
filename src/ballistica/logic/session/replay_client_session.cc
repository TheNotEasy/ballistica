// Released under the MIT License. See LICENSE for details.

#include "ballistica/logic/session/replay_client_session.h"

#include "ballistica/dynamics/material/material.h"
#include "ballistica/generic/huffman.h"
#include "ballistica/generic/utils.h"
#include "ballistica/logic/connection/connection_set.h"
#include "ballistica/logic/connection/connection_to_client.h"
#include "ballistica/math/vector3f.h"
#include "ballistica/networking/networking.h"
#include "ballistica/platform/platform.h"
#include "ballistica/scene/scene.h"
#include "ballistica/scene/scene_stream.h"

namespace ballistica {

auto ReplayClientSession::GetActualTimeAdvance(int advance_in) -> int {
  return static_cast<int>(
      round(advance_in * pow(2.0f, g_logic->replay_speed_exponent())));
}

ReplayClientSession::ReplayClientSession(std::string filename)
    : file_name_(std::move(filename)) {
  // take responsibility for feeding all clients to this device..
  g_logic->connections()->RegisterClientController(this);

  // go ahead and just do a reset here, which will get things going..
  Reset(true);
}

ReplayClientSession::~ReplayClientSession() {
  // we no longer are responsible for feeding clients to this device..
  g_logic->connections()->UnregisterClientController(this);

  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
}

void ReplayClientSession::OnCommandBufferUnderrun() { ResetTargetBaseTime(); }

void ReplayClientSession::OnClientConnected(ConnectionToClient* c) {
  // sanity check - abort if its on either of our lists already
  for (ConnectionToClient* i : connections_to_clients_) {
    if (i == c) {
      Log(LogLevel::kError,
          "ReplayClientSession::OnClientConnected()"
          " got duplicate connection");
      return;
    }
  }
  for (ConnectionToClient* i : connections_to_clients_ignored_) {
    if (i == c) {
      Log(LogLevel::kError,
          "ReplayClientSession::OnClientConnected()"
          " got duplicate connection");
      return;
    }
  }

  // if we've sent *any* commands out to clients so far, we currently have to
  // ignore new connections (need to rebuild state to match current session
  // state)
  {
    connections_to_clients_.push_back(c);

    // we create a temporary output stream just for the purpose of building
    // a giant session-commands message that we can send to the client
    // to build its state up to where we are currently.
    SceneStream out(nullptr, false);

    // go ahead and dump our full state..
    DumpFullState(&out);

    // grab the message that's been built up..
    // if its not empty, send it to the client.
    std::vector<uint8_t> out_message = out.GetOutMessage();
    if (!out_message.empty()) {
      c->SendReliableMessage(out_message);
    }

    // also send a correction packet to sync up all our dynamics
    // (technically could do this *just* for the new client)
    {
      std::vector<std::vector<uint8_t> > messages;
      bool blend = false;
      GetCorrectionMessages(blend, &messages);

      // FIXME - have to send reliably at the moment since these will most
      // likely be bigger than our unreliable packet limit.. :-(
      for (auto&& i : messages) {
        for (auto&& j : connections_to_clients_) {
          j->SendReliableMessage(i);
        }
      }
    }
  }
}

void ReplayClientSession::OnClientDisconnected(ConnectionToClient* c) {
  // Search for it on either our ignored or regular lists.
  for (auto i = connections_to_clients_.begin();
       i != connections_to_clients_.end(); i++) {
    if (*i == c) {
      connections_to_clients_.erase(i);
      return;
    }
  }
  for (auto i = connections_to_clients_ignored_.begin();
       i != connections_to_clients_ignored_.end(); i++) {
    if (*i == c) {
      connections_to_clients_ignored_.erase(i);
      return;
    }
  }
  Log(LogLevel::kError,
      "ReplayClientSession::OnClientDisconnected()"
      " called for connection not on lists");
}

void ReplayClientSession::FetchMessages() {
  if (!file_ || shutting_down()) {
    return;
  }

  // If we have no messages left, read from the file until we get some.
  while (commands().empty()) {
    std::vector<uint8_t> buffer;
    uint8_t len8;
    uint32_t len32;

    // Read the size of the message.
    // the first byte represents the actual size if the value is < 254
    // if it is 254, the 2 bytes after it represent size
    // if it is 255, the 4 bytes after it represent size
    if (fread(&len8, 1, 1, file_) != 1) {
      // So they know to be done when they reach the end of the command list
      // (instead of just waiting for more commands)
      add_end_of_file_command();
      fclose(file_);
      file_ = nullptr;
      return;
    }
    if (len8 < 254) {
      len32 = len8;
    } else {
      // Pull 16 bit len.
      if (len8 == 254) {
        uint16_t len16;
        if (fread(&len16, 2, 1, file_) != 1) {
          // so they know to be done when they reach the end of the command list
          // (instead of just waiting for more commands)
          add_end_of_file_command();
          fclose(file_);
          file_ = nullptr;
          return;
        }
        assert(len16 >= 254);
        len32 = len16;
      } else {
        // Pull 32 bit len.
        if (fread(&len32, 4, 1, file_) != 1) {
          // so they know to be done when they reach the end of the command list
          // (instead of just waiting for more commands)
          add_end_of_file_command();
          fclose(file_);
          file_ = nullptr;
          return;
        }
        assert(len32 > 65535);
      }
    }

    // Read and decompress the actual message.
    BA_PRECONDITION(len32 > 0);
    buffer.resize(len32);
    if (fread(&(buffer[0]), len32, 1, file_) != 1) {
      add_end_of_file_command();
      fclose(file_);
      file_ = nullptr;
      return;
    }
    std::vector<uint8_t> data_decompressed =
        g_utils->huffman()->decompress(buffer);
    HandleSessionMessage(data_decompressed);

    // Also send it to all client-connections we're attached to.
    // NOTE: We currently are sending everything as reliable; we can maybe do
    // unreliable for certain type of messages. Though perhaps when passing
    // around replays maybe its best to keep everything intact.
    have_sent_client_message_ = true;
    for (auto&& i : connections_to_clients_) {
      i->SendReliableMessage(data_decompressed);
    }
    message_fetch_num_++;
  }
}

void ReplayClientSession::Error(const std::string& description) {
  // Close the replay, announce something went wrong with it, and then do
  // standard error response..
  ScreenMessage(g_logic->GetResourceString("replayReadErrorText"), {1, 0, 0});
  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
  ClientSession::Error(description);
}

void ReplayClientSession::OnReset(bool rewind) {
  // Handles base resetting.
  ClientSession::OnReset(rewind);

  // If we've got any clients attached to us, tell them to reset as well.
  for (auto&& i : connections_to_clients_) {
    i->SendReliableMessage(std::vector<uint8_t>(1, BA_MESSAGE_SESSION_RESET));
  }

  // If rewinding, pop back to the start of our file.
  if (rewind) {
    if (file_) {
      fclose(file_);
      file_ = nullptr;
    }

    file_ = g_platform->FOpen(file_name_.c_str(), "rb");
    if (!file_) {
      Error("can't open file for reading");
      return;
    }

    // Read file ID and version to make sure we support this file.
    uint32_t file_id;
    if ((fread(&file_id, sizeof(file_id), 1, file_) != 1)) {
      Error("error reading file_id");
      return;
    }
    if (file_id != kBrpFileID) {
      Error("incorrect file_id");
      return;
    }

    // Make sure its a compatible protocol version.
    uint16_t version;
    if (fread(&version, sizeof(version), 1, file_) != 1) {
      Error("error reading version");
      return;
    }
    if (version > kProtocolVersion || version < kProtocolVersionMin) {
      ScreenMessage(g_logic->GetResourceString("replayVersionErrorText"),
                    {1, 0, 0});
      End();
      return;
    }
  }
}

}  // namespace ballistica