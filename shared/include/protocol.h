#pragma once

#include "decaflash_types.h"

namespace decaflash::protocol {

static constexpr uint16_t kProtocolVersion = 7;
static constexpr uint32_t kProtocolMagic = 0x4443464C;  // DCFL

enum class MessageType : uint8_t {
  FlashCommand = 1,
  RgbCommand = 2,
  NodeStatus = 3,
  ClockSync = 4,
  BrainHello = 5,
};

struct MessageHeader {
  uint32_t magic;
  uint16_t version;
  MessageType type;
  uint8_t reserved;
};

struct FlashCommandMessage {
  MessageHeader header;
  uint32_t commandRevision;
  NodeKind targetNodeKind;
  NodeEffect targetNodeEffect;
  uint8_t reserved0[2];
  FlashCommand command;
};

struct RgbCommandMessage {
  MessageHeader header;
  uint32_t commandRevision;
  NodeKind targetNodeKind;
  NodeEffect targetNodeEffect;
  uint8_t reserved0[2];
  RgbCommand command;
};

struct NodeStatusMessage {
  MessageHeader header;
  NodeIdentity identity;
  uint16_t currentBpm;
  uint8_t beatsPerBar;
  uint8_t currentProgramIndex;
  uint32_t uptimeMs;
};

struct ClockSyncMessage {
  MessageHeader header;
  uint32_t clockRevision;
  uint32_t beatSerial;
  uint16_t bpm;
  uint8_t beatsPerBar;
  uint8_t beatInBar;
  uint32_t currentBar;
};

struct BrainHelloMessage {
  MessageHeader header;
};

constexpr MessageHeader makeHeader(MessageType type) {
  return MessageHeader{
    kProtocolMagic,
    kProtocolVersion,
    type,
    0,
  };
}

constexpr FlashCommandMessage makeFlashCommandMessage(
  NodeKind targetNodeKind,
  NodeEffect targetNodeEffect,
  const FlashCommand& command,
  uint32_t commandRevision
) {
  return FlashCommandMessage{
    makeHeader(MessageType::FlashCommand),
    commandRevision,
    targetNodeKind,
    targetNodeEffect,
    {0, 0},
    command,
  };
}

constexpr RgbCommandMessage makeRgbCommandMessage(
  NodeKind targetNodeKind,
  NodeEffect targetNodeEffect,
  const RgbCommand& command,
  uint32_t commandRevision
) {
  return RgbCommandMessage{
    makeHeader(MessageType::RgbCommand),
    commandRevision,
    targetNodeKind,
    targetNodeEffect,
    {0, 0},
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

constexpr ClockSyncMessage makeClockSyncMessage(
  uint32_t clockRevision,
  uint32_t beatSerial,
  uint16_t bpm,
  uint8_t beatsPerBar,
  uint8_t beatInBar,
  uint32_t currentBar
) {
  return ClockSyncMessage{
    makeHeader(MessageType::ClockSync),
    clockRevision,
    beatSerial,
    bpm,
    beatsPerBar,
    beatInBar,
    currentBar,
  };
}

constexpr BrainHelloMessage makeBrainHelloMessage() {
  return BrainHelloMessage{
    makeHeader(MessageType::BrainHello),
  };
}

}  // namespace decaflash::protocol
