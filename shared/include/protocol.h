#pragma once

#include "decaflash_types.h"

namespace decaflash::protocol {

static constexpr uint16_t kProtocolVersion = 11;
static constexpr uint32_t kProtocolMagic = 0x4443464C;  // DCFL
static constexpr size_t kNodeTextLength = 48;

enum class MessageType : uint8_t {
  FlashCommand = 1,
  RgbCommand = 2,
  NodeStatus = 3,
  ClockSync = 4,
  BrainHello = 5,
  NodeText = 6,
};

struct MessageHeader {
  uint32_t magic;
  uint16_t version;
  MessageType type;
  uint8_t reserved;
};

enum NodeClockSyncFlags : uint8_t {
  kNodeClockSyncFlagMeasured = 1 << 0,
  kNodeClockSyncFlagDuplicateBeat = 1 << 1,
  kNodeClockSyncFlagPredictedBeat = 1 << 2,
  kNodeClockSyncFlagResync = 1 << 3,
};

struct NodeClockSyncTelemetry {
  uint32_t clockRevision;
  uint32_t beatSerial;
  uint32_t currentBar;
  int16_t phaseErrorMs;
  uint8_t beatInBar;
  uint8_t flags;
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
  NodeClockSyncTelemetry clockSync;
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

enum NodeTextFlags : uint8_t {
  kNodeTextFlagCancel = 1 << 0,
};

struct NodeTextMessage {
  MessageHeader header;
  uint32_t textRevision;
  NodeKind targetNodeKind;
  uint8_t flags;
  uint8_t reserved0[2];
  char text[kNodeTextLength];
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
  uint32_t uptimeMs,
  NodeClockSyncTelemetry clockSync = {}
) {
  return NodeStatusMessage{
    makeHeader(MessageType::NodeStatus),
    identity,
    currentBpm,
    beatsPerBar,
    currentProgramIndex,
    uptimeMs,
    clockSync,
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

inline NodeTextMessage makeNodeTextMessage(
  NodeKind targetNodeKind,
  uint32_t textRevision,
  const char* text,
  uint8_t flags = 0
) {
  NodeTextMessage message = {};
  message.header = makeHeader(MessageType::NodeText);
  message.textRevision = textRevision;
  message.targetNodeKind = targetNodeKind;
  message.flags = flags;

  size_t index = 0;
  while (text != nullptr && text[index] != '\0' && index + 1U < kNodeTextLength) {
    message.text[index] = text[index];
    ++index;
  }

  while (index < kNodeTextLength) {
    message.text[index++] = '\0';
  }

  return message;
}

}  // namespace decaflash::protocol
