#pragma once

#include "decaflash_types.h"

namespace decaflash::protocol {

static constexpr uint16_t kProtocolVersion = 1;
static constexpr uint32_t kProtocolMagic = 0x4443464C;  // DCFL

enum class MessageType : uint8_t {
  NodeCommand = 1,
  NodeStatus = 2,
};

struct MessageHeader {
  uint32_t magic;
  uint16_t version;
  MessageType type;
  uint8_t reserved;
};

struct NodeCommandMessage {
  MessageHeader header;
  uint32_t commandRevision;
  uint16_t bpm;
  uint8_t beatsPerBar;
  uint8_t reserved0;
  NodeKind targetNodeKind;
  uint8_t reserved1[3];
  NodeCommand command;
};

struct NodeStatusMessage {
  MessageHeader header;
  NodeIdentity identity;
  uint16_t currentBpm;
  uint8_t beatsPerBar;
  uint8_t currentProgramIndex;
  uint32_t uptimeMs;
};

constexpr MessageHeader makeHeader(MessageType type) {
  return MessageHeader{
    kProtocolMagic,
    kProtocolVersion,
    type,
    0,
  };
}

constexpr NodeCommandMessage makeNodeCommandMessage(
  NodeKind targetNodeKind,
  const NodeCommand& command,
  uint16_t bpm,
  uint8_t beatsPerBar,
  uint32_t commandRevision
) {
  return NodeCommandMessage{
    makeHeader(MessageType::NodeCommand),
    commandRevision,
    bpm,
    beatsPerBar,
    0,
    targetNodeKind,
    {0, 0, 0},
    command,
  };
}

constexpr NodeStatusMessage makeNodeStatusMessage(
  NodeIdentity identity,
  uint16_t currentBpm,
  uint8_t beatsPerBar,
  uint8_t currentProgramIndex,
  uint32_t uptimeMs
) {
  return NodeStatusMessage{
    makeHeader(MessageType::NodeStatus),
    identity,
    currentBpm,
    beatsPerBar,
    currentProgramIndex,
    uptimeMs,
  };
}

}  // namespace decaflash::protocol
