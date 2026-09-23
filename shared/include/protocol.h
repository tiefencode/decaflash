#pragma once

#include "decaflash_types.h"

namespace decaflash::protocol {

static constexpr uint16_t kProtocolVersion = 13;
static constexpr uint32_t kProtocolMagic = 0x4443464C;  // DCFL
static constexpr size_t kNodeTextLength = 48;

enum class MessageType : uint8_t {
  SceneSelect = 1,
  ClockSync = 2,
  BrainHello = 3,
  NodeText = 4,
};

struct MessageHeader {
  uint32_t magic;
  uint16_t version;
  MessageType type;
  uint8_t reserved;
};

// Scenes are compiled into both Brain and Nodes. The Brain only selects one;
// each Node derives its role-specific command locally.
struct SceneSelectMessage {
  MessageHeader header;
  uint8_t sceneIndex;
  uint8_t reserved0[3];
};

struct ClockSyncMessage {
  MessageHeader header;
  uint16_t bpm;
  uint8_t beatsPerBar;
  uint8_t beatInBar;
  uint32_t currentBar;
};

// Hello is deliberately a one-shot greeting. It has no session or revision.
struct BrainHelloMessage {
  MessageHeader header;
};

enum NodeTextFlags : uint8_t {
  kNodeTextFlagCancel = 1 << 0,
};

struct NodeTextMessage {
  MessageHeader header;
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

constexpr SceneSelectMessage makeSceneSelectMessage(uint8_t sceneIndex) {
  return SceneSelectMessage{
    makeHeader(MessageType::SceneSelect),
    sceneIndex,
    {0, 0, 0},
  };
}

constexpr ClockSyncMessage makeClockSyncMessage(
  uint16_t bpm,
  uint8_t beatsPerBar,
  uint8_t beatInBar,
  uint32_t currentBar
) {
  return ClockSyncMessage{
    makeHeader(MessageType::ClockSync),
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
  const char* text,
  uint8_t flags = 0
) {
  NodeTextMessage message = {};
  message.header = makeHeader(MessageType::NodeText);
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
