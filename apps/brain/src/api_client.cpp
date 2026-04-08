#include "api_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "text_playback.h"
#include "wifi_manager.h"

#if __has_include("cloud_config.h")
#include "cloud_config.h"
#define DECAFLASH_CLOUD_CONFIG_AVAILABLE 1
#else
#define DECAFLASH_CLOUD_CONFIG_AVAILABLE 0
namespace decaflash::secrets {
static constexpr char kCloudChattieUrl[] = "";
static constexpr char kBrainSharedSecret[] = "";
}  // namespace decaflash::secrets
#endif

namespace decaflash::brain::api_client {

namespace {

static constexpr size_t kCloudTextCapacity = 96;
static constexpr size_t kCloudTitleCapacity = 96;
static constexpr size_t kCloudArtistCapacity = 96;
static constexpr size_t kCloudInputCapacity = 96;
static constexpr size_t kCloudHttpBodyCapacity = 512;
static constexpr size_t kCloudJsonPayloadCapacity = 512;
static constexpr uint32_t kCloudTextDisplayDelayMs = 320;
static constexpr uint32_t kCloudWorkerStackWords = 12288;
static constexpr BaseType_t kCloudWorkerPriority = 1;
// Keep Wi-Fi join tolerant, but fail cloud work faster so AI does not hold the
// ESP-NOW core path paused for too long.
static constexpr uint16_t kCloudConnectTimeoutMs = 8000;
static constexpr uint16_t kCloudRequestTimeoutMs = 15000;
static constexpr uint16_t kCloudResponseIdleTimeoutMs = 500;
static constexpr char kCloudChattieMonitorPrompt[] =
  "Schreibe genau einen sehr kurzen kreativen deutschen Text fuer eine 5x5-LED-Matrix. "
  "Nutze die Nutzereingabe als Inspiration. Keine Erklaerung. Keine Emojis. "
  "Maximal 8 Woerter.";
static constexpr char kCloudMultipartBoundary[] = "----decaflash-boundary-7e8db64a";

enum class CloudJobType : uint8_t {
  None = 0,
  Prompt = 1,
  RecordedAudio = 2,
};

enum class CloudJobOwner : uint8_t {
  Manual = 0,
  Ai = 1,
};

struct CloudQueuedJob {
  CloudJobType type = CloudJobType::None;
  CloudJobOwner owner = CloudJobOwner::Manual;
  uint32_t aiGeneration = 0;
  char input[kCloudInputCapacity] = {};
  RecordedAudioClip recording = {};
};

class MultipartClipStream final : public Stream {
 public:
  MultipartClipStream(const char* preamble,
                      size_t preambleLength,
                      const RecordedAudioClip& recording,
                      const char* footer,
                      size_t footerLength)
    : preamble_(reinterpret_cast<const uint8_t*>(preamble)),
      preambleLength_(preambleLength),
      clipData_(recording.data),
      clipDataLength_(recording.byteCount),
      footer_(reinterpret_cast<const uint8_t*>(footer)),
      footerLength_(footerLength),
      totalLength_(preambleLength_ + clipDataLength_ + footerLength_) {
  }

  int available() override {
    return (position_ < totalLength_) ? static_cast<int>(totalLength_ - position_) : 0;
  }

  int read() override {
    uint8_t byte = 0;
    return (readBytes(&byte, 1) == 1) ? byte : -1;
  }

  int peek() override {
    if (position_ >= totalLength_) {
      return -1;
    }

    return byteAt(position_);
  }

  void flush() override {
  }

  size_t write(uint8_t) override {
    return 0;
  }

  size_t readBytes(char* buffer, size_t length) override {
    return readBytes(reinterpret_cast<uint8_t*>(buffer), length);
  }

  size_t readBytes(uint8_t* buffer, size_t length) override {
    if (buffer == nullptr || length == 0 || position_ >= totalLength_) {
      return 0;
    }

    size_t copied = 0;
    while (copied < length && position_ < totalLength_) {
      const Section section = activeSection();
      if (section.length == 0) {
        break;
      }

      if (section.data == nullptr) {
        break;
      }

      const size_t sectionOffset = position_ - section.startOffset;
      const size_t bytesRemaining = section.length - sectionOffset;
      const size_t chunkLength = ((length - copied) < bytesRemaining)
                                   ? (length - copied)
                                   : bytesRemaining;

      memcpy(buffer + copied, section.data + sectionOffset, chunkLength);
      copied += chunkLength;
      position_ += chunkLength;
    }

    return copied;
  }

  size_t totalLength() const {
    return totalLength_;
  }

 private:
  struct Section {
    const uint8_t* data;
    size_t startOffset;
    size_t length;
  };

  int byteAt(size_t absoluteOffset) const {
    (void)absoluteOffset;
    return -1;
  }

  Section activeSection() const {
    return sectionForOffset(position_);
  }

  Section sectionForOffset(size_t absoluteOffset) const {
    if (absoluteOffset < preambleLength_) {
      return {preamble_, 0, preambleLength_};
    }

    const size_t clipOffset = preambleLength_;
    if (absoluteOffset < (clipOffset + clipDataLength_)) {
      return {clipData_, clipOffset, clipDataLength_};
    }

    const size_t footerOffset = clipOffset + clipDataLength_;
    if (absoluteOffset < (footerOffset + footerLength_)) {
      return {footer_, footerOffset, footerLength_};
    }

    return {nullptr, totalLength_, 0};
  }

  const uint8_t* preamble_;
  size_t preambleLength_;
  const uint8_t* clipData_;
  size_t clipDataLength_;
  const uint8_t* footer_;
  size_t footerLength_;
  size_t totalLength_;
  size_t position_ = 0;
};

bool fetchCloudChattieText(const char* input,
                           const char* instructions,
                           char* destination,
                           size_t capacity);

TaskHandle_t cloudWorkerTaskHandle = nullptr;
portMUX_TYPE cloudJobMux = portMUX_INITIALIZER_UNLOCKED;
CloudQueuedJob queuedCloudJob = {};
bool cloudJobQueued = false;
bool cloudJobRunning = false;
bool cloudWorkerReady = false;
char completedDisplayText[kCloudTextCapacity] = {};
bool completedDisplayPending = false;
uint32_t completedDisplayAtMs = 0;
CloudJobOwner completedDisplayOwner = CloudJobOwner::Manual;
bool recordedAudioCompletionPending = false;
bool recordedAudioProcessed = false;
bool recordedAudioWifiFailed = false;
CloudJobOwner recordedAudioCompletionOwner = CloudJobOwner::Manual;
uint32_t aiCancelGeneration = 1;
bool managedWifiSessionActive = false;

void clearQueuedJobLocked() {
  queuedCloudJob.type = CloudJobType::None;
  queuedCloudJob.owner = CloudJobOwner::Manual;
  queuedCloudJob.aiGeneration = 0;
  queuedCloudJob.input[0] = '\0';
  queuedCloudJob.recording = {};
}

bool cloudConfigured() {
#if DECAFLASH_CLOUD_CONFIG_AVAILABLE
  return decaflash::secrets::kCloudChattieUrl[0] != '\0' &&
         decaflash::secrets::kBrainSharedSecret[0] != '\0';
#else
  return false;
#endif
}

bool ensureWifiConnected() {
  if (decaflash::brain::wifi_manager::isConnected()) {
    return true;
  }

  if (decaflash::brain::wifi_manager::connect()) {
    return true;
  }

  Serial.println("API: abort reason=wifi_not_connected");
  return false;
}

bool copyInputText(const char* input, char* destination, size_t capacity) {
  if (input == nullptr || destination == nullptr || capacity == 0) {
    return false;
  }

  const size_t inputLength = strnlen(input, capacity);
  if (inputLength == 0 || inputLength >= capacity) {
    return false;
  }

  memcpy(destination, input, inputLength);
  destination[inputLength] = '\0';
  return true;
}

bool appendCharacter(char* destination, size_t capacity, size_t& length, char value) {
  if ((length + 1U) >= capacity) {
    return false;
  }

  destination[length++] = value;
  destination[length] = '\0';
  return true;
}

bool appendText(char* destination, size_t capacity, size_t& length, const char* source) {
  if (destination == nullptr || source == nullptr || capacity == 0) {
    return false;
  }

  while (*source != '\0') {
    if (!appendCharacter(destination, capacity, length, *source)) {
      return false;
    }
    source++;
  }

  return true;
}

void printResponseBodySnippet(const char* label, const char* body) {
  if (label == nullptr || body == nullptr || body[0] == '\0') {
    return;
  }

  char snippet[184] = {};
  size_t length = 0;
  const char* cursor = body;
  while (*cursor != '\0' && length < (sizeof(snippet) - 4U)) {
    char value = *cursor++;
    if (value == '\r' || value == '\n') {
      value = ' ';
    }
    snippet[length++] = value;
  }
  if (*cursor != '\0') {
    snippet[length++] = '.';
    snippet[length++] = '.';
    snippet[length++] = '.';
  }
  snippet[length] = '\0';

  Serial.printf("API: %s_body body=\"%s\"\n", label, snippet);
}

bool extractJsonStringField(const char* body,
                            const char* fieldName,
                            char* destination,
                            size_t capacity) {
  if (body == nullptr || fieldName == nullptr || destination == nullptr || capacity == 0) {
    return false;
  }

  destination[0] = '\0';
  char fieldPattern[40] = {};
  snprintf(fieldPattern, sizeof(fieldPattern), "\"%s\"", fieldName);

  const char* keyStart = strstr(body, fieldPattern);
  if (keyStart == nullptr) {
    return false;
  }

  const char* colon = strchr(keyStart, ':');
  if (colon == nullptr) {
    return false;
  }

  const char* cursor = colon + 1;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }

  if (*cursor != '"') {
    return false;
  }

  cursor++;
  size_t length = 0;
  while (*cursor != '\0' && *cursor != '"') {
    if (*cursor == '\\') {
      cursor++;
      if (*cursor == '\0') {
        break;
      }

      char decoded = *cursor;
      switch (*cursor) {
        case 'n':
        case 'r':
        case 't':
          decoded = ' ';
          break;
        case '"':
        case '\\':
        case '/':
          decoded = *cursor;
          break;
        default:
          decoded = *cursor;
          break;
      }

      if (!appendCharacter(destination, capacity, length, decoded)) {
        return false;
      }
      cursor++;
      continue;
    }

    if (!appendCharacter(destination, capacity, length, *cursor)) {
      return false;
    }
    cursor++;
  }

  return length > 0;
}

bool extractJsonBoolField(const char* body, const char* fieldName, bool& value) {
  if (body == nullptr || fieldName == nullptr) {
    return false;
  }

  char fieldPattern[40] = {};
  snprintf(fieldPattern, sizeof(fieldPattern), "\"%s\"", fieldName);

  const char* keyStart = strstr(body, fieldPattern);
  if (keyStart == nullptr) {
    return false;
  }

  const char* colon = strchr(keyStart, ':');
  if (colon == nullptr) {
    return false;
  }

  const char* cursor = colon + 1;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }

  if (strncmp(cursor, "true", 4) == 0) {
    value = true;
    return true;
  }

  if (strncmp(cursor, "false", 5) == 0) {
    value = false;
    return true;
  }

  return false;
}

bool appendJsonEscaped(char* destination, size_t capacity, size_t& length, const char* source) {
  if (destination == nullptr || source == nullptr || capacity == 0) {
    return false;
  }

  while (*source != '\0') {
    switch (*source) {
      case '\\':
        if (!appendText(destination, capacity, length, "\\\\")) {
          return false;
        }
        break;

      case '"':
        if (!appendText(destination, capacity, length, "\\\"")) {
          return false;
        }
        break;

      case '\n':
        if (!appendText(destination, capacity, length, "\\n")) {
          return false;
        }
        break;

      case '\r':
        if (!appendText(destination, capacity, length, "\\r")) {
          return false;
        }
        break;

      case '\t':
        if (!appendText(destination, capacity, length, "\\t")) {
          return false;
        }
        break;

      default:
        if (!appendCharacter(destination, capacity, length, *source)) {
          return false;
        }
        break;
    }

    source++;
  }

  return true;
}

size_t readHttpResponseBody(HTTPClient& http, char* destination, size_t capacity) {
  if (destination == nullptr || capacity == 0) {
    return 0;
  }

  destination[0] = '\0';

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return 0;
  }

  size_t length = 0;
  const int expectedLength = http.getSize();
  const size_t maximumLength =
    (expectedLength > 0 && static_cast<size_t>(expectedLength) < (capacity - 1U))
      ? static_cast<size_t>(expectedLength)
      : (capacity - 1U);
  uint32_t lastProgressAt = millis();

  while ((http.connected() || stream->available()) && length < maximumLength) {
    const int availableBytes = stream->available();
    if (availableBytes <= 0) {
      if (expectedLength > 0 && length >= static_cast<size_t>(expectedLength)) {
        break;
      }
      if ((millis() - lastProgressAt) >= kCloudResponseIdleTimeoutMs) {
        break;
      }
      delay(1);
      continue;
    }

    const size_t chunkLength = (static_cast<size_t>(availableBytes) < (maximumLength - length))
                                 ? static_cast<size_t>(availableBytes)
                                 : (maximumLength - length);
    const size_t bytesRead = stream->readBytes(destination + length, chunkLength);
    if (bytesRead == 0) {
      if ((millis() - lastProgressAt) >= kCloudResponseIdleTimeoutMs) {
        break;
      }
      delay(1);
      continue;
    }

    length += bytesRead;
    lastProgressAt = millis();
  }

  destination[length] = '\0';
  return length;
}

bool beginSecureRequest(HTTPClient& http,
                        WiFiClientSecure& client,
                        const char* url,
                        const char* label) {
  client.setInsecure();
  client.setTimeout(kCloudRequestTimeoutMs);
  if (!http.begin(client, url)) {
    Serial.printf("API: begin_failed endpoint=%s\n", label);
    return false;
  }

  http.setConnectTimeout(kCloudConnectTimeoutMs);
  http.setTimeout(kCloudRequestTimeoutMs);
  http.setReuse(false);
  http.addHeader("User-Agent", "decaflash-brain");
  http.addHeader("Accept", "application/json, text/plain");
  http.addHeader("Connection", "close");
  return true;
}

void addCloudAuthorizationHeader(HTTPClient& http) {
  char bearerValue[160] = {};
  snprintf(bearerValue,
           sizeof(bearerValue),
           "Bearer %s",
           decaflash::secrets::kBrainSharedSecret);
  http.addHeader("Authorization", bearerValue);
}

bool deriveCloudAuddUrl(char* destination, size_t capacity) {
  if (capacity == 0) {
    return false;
  }

  destination[0] = '\0';
  const char* chattieUrl = decaflash::secrets::kCloudChattieUrl;
  const char* suffix = strstr(chattieUrl, "/api/chattie");
  if (suffix == nullptr || strcmp(suffix, "/api/chattie") != 0) {
    Serial.println("API: cloud_config_invalid expected=kCloudChattieUrl_ends_with_/api/chattie");
    return false;
  }

  const size_t prefixLength = static_cast<size_t>(suffix - chattieUrl);
  const size_t requiredLength = prefixLength + strlen("/api/audd");
  if (requiredLength >= capacity) {
    return false;
  }

  memcpy(destination, chattieUrl, prefixLength);
  memcpy(destination + prefixLength, "/api/audd", strlen("/api/audd") + 1U);
  return true;
}

void writeLe16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFFU);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void writeLe32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFFU);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  destination[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  destination[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

void buildWavHeader(uint8_t* destination,
                    size_t sampleCount,
                    uint32_t sampleRateHz,
                    uint16_t channelCount,
                    uint16_t bitsPerSample) {
  const uint32_t byteRate = sampleRateHz * channelCount * (bitsPerSample / 8U);
  const uint16_t blockAlign = static_cast<uint16_t>(channelCount * (bitsPerSample / 8U));
  const uint32_t pcmDataSize = static_cast<uint32_t>(sampleCount * blockAlign);
  const uint32_t riffChunkSize = 36U + pcmDataSize;

  memcpy(destination + 0, "RIFF", 4);
  writeLe32(destination + 4, riffChunkSize);
  memcpy(destination + 8, "WAVE", 4);
  memcpy(destination + 12, "fmt ", 4);
  writeLe32(destination + 16, 16);
  writeLe16(destination + 20, 1);
  writeLe16(destination + 22, channelCount);
  writeLe32(destination + 24, sampleRateHz);
  writeLe32(destination + 28, byteRate);
  writeLe16(destination + 32, blockAlign);
  writeLe16(destination + 34, bitsPerSample);
  memcpy(destination + 36, "data", 4);
  writeLe32(destination + 40, pcmDataSize);
}

bool postCloudJson(HTTPClient& http,
                   WiFiClientSecure& client,
                   const char* url,
                   const char* label,
                   const char* payload,
                   char* responseBody,
                   size_t responseCapacity) {
  if (!beginSecureRequest(http, client, url, label)) {
    return false;
  }

  addCloudAuthorizationHeader(http);
  http.addHeader("Content-Type", "application/json");

  const int statusCode = http.POST(payload);
  if (statusCode <= 0) {
    Serial.printf("API: request_failed endpoint=%s err=%s\n",
                  label,
                  http.errorToString(statusCode).c_str());
    http.end();
    return false;
  }

  const size_t responseLength = readHttpResponseBody(http, responseBody, responseCapacity);
  http.end();

  if (statusCode != HTTP_CODE_OK || responseLength == 0) {
    printResponseBodySnippet(label, responseBody);
    Serial.printf("API: %s_failed status=%d bytes=%u\n",
                  label,
                  statusCode,
                  static_cast<unsigned>(responseLength));
    return false;
  }

  return true;
}

bool postCloudStream(HTTPClient& http,
                     WiFiClientSecure& client,
                     const char* url,
                     const char* label,
                     const char* contentType,
                     Stream& stream,
                     size_t contentLength,
                     char* responseBody,
                     size_t responseCapacity) {
  if (!beginSecureRequest(http, client, url, label)) {
    return false;
  }

  addCloudAuthorizationHeader(http);
  http.addHeader("Content-Type", contentType);

  const int statusCode = http.sendRequest("POST", &stream, contentLength);
  if (statusCode <= 0) {
    Serial.printf("API: request_failed endpoint=%s err=%s\n",
                  label,
                  http.errorToString(statusCode).c_str());
    http.end();
    return false;
  }

  const size_t responseLength = readHttpResponseBody(http, responseBody, responseCapacity);
  http.end();

  if (statusCode != HTTP_CODE_OK || responseLength == 0) {
    printResponseBodySnippet(label, responseBody);
    Serial.printf("API: %s_failed status=%d bytes=%u\n",
                  label,
                  statusCode,
                  static_cast<unsigned>(responseLength));
    return false;
  }

  return true;
}

bool fetchCloudSongTextFromRecording(const RecordedAudioClip& recording,
                                     char* destination,
                                     size_t capacity) {
  if (recording.data == nullptr || recording.byteCount == 0 ||
      recording.sampleCount == 0 || recording.sampleRateHz == 0 ||
      destination == nullptr || capacity == 0) {
    return false;
  }

  destination[0] = '\0';

  char auddUrl[160] = {};
  if (!deriveCloudAuddUrl(auddUrl, sizeof(auddUrl))) {
    Serial.println("API: audd_url_invalid");
    return false;
  }

  char preamble[640] = {};
  const int preambleLength = snprintf(
    preamble,
    sizeof(preamble),
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"encoding\"\r\n"
    "\r\n"
    "mulaw\r\n"
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"sample_rate_hz\"\r\n"
    "\r\n"
    "%lu\r\n"
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"sample_count\"\r\n"
    "\r\n"
    "%u\r\n"
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"container\"\r\n"
    "\r\n"
    "decaflash_mulaw\r\n"
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"recording.mulaw\"\r\n"
    "Content-Type: application/octet-stream\r\n"
    "\r\n",
    kCloudMultipartBoundary,
    kCloudMultipartBoundary,
    static_cast<unsigned long>(recording.sampleRateHz),
    kCloudMultipartBoundary,
    static_cast<unsigned>(recording.sampleCount),
    kCloudMultipartBoundary,
    kCloudMultipartBoundary
  );
  if (preambleLength <= 0 || static_cast<size_t>(preambleLength) >= sizeof(preamble)) {
    Serial.printf("API: audd_preamble_invalid len=%d cap=%u\n",
                  preambleLength,
                  static_cast<unsigned>(sizeof(preamble)));
    return false;
  }

  char footer[64] = {};
  const int footerLength = snprintf(
    footer,
    sizeof(footer),
    "\r\n--%s--\r\n",
    kCloudMultipartBoundary
  );
  if (footerLength <= 0 || static_cast<size_t>(footerLength) >= sizeof(footer)) {
    Serial.printf("API: audd_footer_invalid len=%d cap=%u\n",
                  footerLength,
                  static_cast<unsigned>(sizeof(footer)));
    return false;
  }

  MultipartClipStream stream(
    preamble,
    static_cast<size_t>(preambleLength),
    recording,
    footer,
    static_cast<size_t>(footerLength)
  );

  char contentType[96] = {};
  snprintf(contentType,
           sizeof(contentType),
           "multipart/form-data; boundary=%s",
           kCloudMultipartBoundary);

  WiFiClientSecure client;
  HTTPClient http;
  char body[kCloudHttpBodyCapacity] = {};
  if (!postCloudStream(http,
                       client,
                       auddUrl,
                       "audd",
                       contentType,
                       stream,
                       stream.totalLength(),
                       body,
                       sizeof(body))) {
    return false;
  }

  bool matched = false;
  if (!extractJsonBoolField(body, "matched", matched)) {
    Serial.println("API: audd_parse_failed expected=matched_bool");
    return false;
  }

  if (!matched) {
    Serial.println("API: audd_no_match");
    return false;
  }

  char title[kCloudTitleCapacity] = {};
  char artist[kCloudArtistCapacity] = {};
  if (!extractJsonStringField(body, "title", title, sizeof(title)) ||
      !extractJsonStringField(body, "artist", artist, sizeof(artist))) {
    Serial.println("API: audd_parse_failed expected=title_and_artist");
    return false;
  }

  if (!extractJsonStringField(body, "text", destination, capacity)) {
    Serial.println("API: audd_parse_failed expected=text");
    return false;
  }

  Serial.printf("API: audd_match artist=\"%s\" title=\"%s\"\n", artist, title);
  Serial.printf("API: audd_text text=\"%s\"\n", destination);
  return true;
}

bool fetchCloudChattieText(const char* input,
                           const char* instructions,
                           char* destination,
                           size_t capacity) {
  if (capacity == 0 || input == nullptr || instructions == nullptr) {
    return false;
  }

  destination[0] = '\0';

  char payload[kCloudJsonPayloadCapacity] = {};
  size_t payloadLength = 0;
  if (!appendText(payload, sizeof(payload), payloadLength, "{\"instructions\":\"") ||
      !appendJsonEscaped(payload, sizeof(payload), payloadLength, instructions) ||
      !appendText(payload, sizeof(payload), payloadLength, "\",\"input\":\"") ||
      !appendJsonEscaped(payload, sizeof(payload), payloadLength, input) ||
      !appendText(payload, sizeof(payload), payloadLength, "\"}")) {
    Serial.println("API: chattie_payload_too_large");
    return false;
  }

  WiFiClientSecure client;
  HTTPClient http;
  char body[kCloudHttpBodyCapacity] = {};
  if (!postCloudJson(http,
                     client,
                     decaflash::secrets::kCloudChattieUrl,
                     "chattie",
                     payload,
                     body,
                     sizeof(body))) {
    return false;
  }

  if (!extractJsonStringField(body, "text", destination, capacity)) {
    Serial.println("API: chattie_parse_failed expected=text");
    return false;
  }

  Serial.printf("API: chattie_text text=\"%s\"\n", destination);
  return true;
}

void cloudWorkerTask(void* parameter) {
  (void)parameter;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    CloudQueuedJob localJob = {};
    bool haveJob = false;
    portENTER_CRITICAL(&cloudJobMux);
    if (cloudJobQueued) {
      localJob = queuedCloudJob;
      clearQueuedJobLocked();
      cloudJobQueued = false;
      cloudJobRunning = true;
      haveJob = true;
    }
    portEXIT_CRITICAL(&cloudJobMux);

    if (!haveJob) {
      continue;
    }

    bool processed = false;
    char displayText[kCloudTextCapacity] = {};

    if (!ensureWifiConnected()) {
      releaseRecordedAudioClip(localJob.recording);

      const bool aiJobCanceled =
        (localJob.owner == CloudJobOwner::Ai) &&
        (localJob.aiGeneration != aiCancelGeneration);

      portENTER_CRITICAL(&cloudJobMux);
      cloudJobRunning = false;
      if (!aiJobCanceled && localJob.type == CloudJobType::RecordedAudio) {
        recordedAudioCompletionPending = true;
        recordedAudioProcessed = false;
        recordedAudioWifiFailed = true;
        recordedAudioCompletionOwner = localJob.owner;
      }
      portEXIT_CRITICAL(&cloudJobMux);
      continue;
    }

    switch (localJob.type) {
      case CloudJobType::Prompt:
        processed = fetchCloudChattieText(
          localJob.input,
          kCloudChattieMonitorPrompt,
          displayText,
          sizeof(displayText));
        break;

      case CloudJobType::RecordedAudio: {
        processed = fetchCloudSongTextFromRecording(
              localJob.recording,
              displayText,
              sizeof(displayText));
        break;
      }

      case CloudJobType::None:
      default:
        break;
    }

    releaseRecordedAudioClip(localJob.recording);

    const bool aiJobCanceled =
      (localJob.owner == CloudJobOwner::Ai) &&
      (localJob.aiGeneration != aiCancelGeneration);

    portENTER_CRITICAL(&cloudJobMux);
    cloudJobRunning = false;
    if (!aiJobCanceled && processed && displayText[0] != '\0') {
      memcpy(completedDisplayText, displayText, sizeof(completedDisplayText));
      completedDisplayPending = true;
      completedDisplayAtMs = millis() + kCloudTextDisplayDelayMs;
      completedDisplayOwner = localJob.owner;
    }
    if (!aiJobCanceled && localJob.type == CloudJobType::RecordedAudio) {
      recordedAudioCompletionPending = true;
      recordedAudioProcessed = processed;
      recordedAudioWifiFailed = false;
      recordedAudioCompletionOwner = localJob.owner;
    }
    portEXIT_CRITICAL(&cloudJobMux);
  }
}

bool ensureCloudWorkerReady() {
  if (cloudWorkerReady && cloudWorkerTaskHandle != nullptr) {
    return true;
  }

  BaseType_t created = xTaskCreatePinnedToCore(
    cloudWorkerTask,
    "cloud_worker",
    kCloudWorkerStackWords,
    nullptr,
    kCloudWorkerPriority,
    &cloudWorkerTaskHandle,
    0);

  if (created != pdPASS || cloudWorkerTaskHandle == nullptr) {
    Serial.println("API: worker_create_failed");
    cloudWorkerTaskHandle = nullptr;
    return false;
  }

  cloudWorkerReady = true;
  return true;
}

bool queueCloudJob(CloudQueuedJob& job) {
  if (!ensureCloudWorkerReady()) {
    releaseRecordedAudioClip(job.recording);
    return false;
  }

  if (decaflash::brain::text_playback::isActive()) {
    releaseRecordedAudioClip(job.recording);
    return false;
  }

  portENTER_CRITICAL(&cloudJobMux);
  const bool jobBusy = cloudJobQueued || cloudJobRunning || completedDisplayPending;
  if (!jobBusy) {
    queuedCloudJob = job;
    cloudJobQueued = true;
    managedWifiSessionActive = true;
  }
  portEXIT_CRITICAL(&cloudJobMux);

  if (jobBusy) {
    releaseRecordedAudioClip(job.recording);
    return false;
  }

  xTaskNotifyGive(cloudWorkerTaskHandle);
  return true;
}

}  // namespace

void begin() {
  (void)ensureCloudWorkerReady();
}

void service(uint32_t now) {
  bool shouldStopManagedWifiSession = false;
  bool shouldStartText = false;
  char textBuffer[kCloudTextCapacity] = {};

  portENTER_CRITICAL(&cloudJobMux);
  shouldStopManagedWifiSession =
    managedWifiSessionActive &&
    !cloudJobQueued &&
    !cloudJobRunning;
  if (completedDisplayPending &&
      !decaflash::brain::text_playback::isActive() &&
      static_cast<int32_t>(now - completedDisplayAtMs) >= 0) {
    memcpy(textBuffer, completedDisplayText, sizeof(textBuffer));
    completedDisplayPending = false;
    completedDisplayText[0] = '\0';
    completedDisplayAtMs = 0;
    shouldStartText = textBuffer[0] != '\0';
  }
  portEXIT_CRITICAL(&cloudJobMux);

  if (shouldStopManagedWifiSession) {
    decaflash::brain::wifi_manager::disconnect();
    managedWifiSessionActive = false;
    Serial.println("API: wifi_session stopped");
  }

  if (shouldStartText) {
    decaflash::brain::text_playback::start(textBuffer);
  }
}

bool busy() {
  if (decaflash::brain::text_playback::isActive()) {
    return true;
  }

  bool isBusy = false;
  portENTER_CRITICAL(&cloudJobMux);
  isBusy = cloudJobQueued || cloudJobRunning || completedDisplayPending;
  portEXIT_CRITICAL(&cloudJobMux);
  return isBusy;
}

bool radioPauseActive() {
  return managedWifiSessionActive;
}

void cancelAiWork() {
  portENTER_CRITICAL(&cloudJobMux);
  aiCancelGeneration++;

  if (cloudJobQueued && queuedCloudJob.owner == CloudJobOwner::Ai) {
    releaseRecordedAudioClip(queuedCloudJob.recording);
    clearQueuedJobLocked();
    cloudJobQueued = false;
  }

  if (completedDisplayPending && completedDisplayOwner == CloudJobOwner::Ai) {
    completedDisplayPending = false;
    completedDisplayText[0] = '\0';
    completedDisplayAtMs = 0;
    completedDisplayOwner = CloudJobOwner::Manual;
  }

  if (recordedAudioCompletionPending && recordedAudioCompletionOwner == CloudJobOwner::Ai) {
    recordedAudioCompletionPending = false;
    recordedAudioProcessed = false;
    recordedAudioWifiFailed = false;
    recordedAudioCompletionOwner = CloudJobOwner::Manual;
  }
  portEXIT_CRITICAL(&cloudJobMux);
}

bool queueCloudChattieInputToTextDisplay(const char* input) {
  if (!cloudConfigured()) {
    Serial.println("API: cloud_missing_config file=include/cloud_config.h");
    Serial.println("API: cloud_expected keys=kCloudChattieUrl,kBrainSharedSecret");
    return false;
  }

  if (input == nullptr || input[0] == '\0') {
    Serial.println("API: chattie_missing_input");
    return false;
  }

  CloudQueuedJob job = {};
  job.type = CloudJobType::Prompt;
  job.owner = CloudJobOwner::Manual;
  if (!copyInputText(input, job.input, sizeof(job.input))) {
    Serial.println("API: chattie_input_invalid");
    return false;
  }

  return queueCloudJob(job);
}

bool queueRecordedAudioToTextDisplay(RecordedAudioClip& recording, bool aiOwned) {
  if (!cloudConfigured()) {
    Serial.println("API: cloud_missing_config file=include/cloud_config.h");
    Serial.println("API: cloud_expected keys=kCloudChattieUrl,kBrainSharedSecret");
    releaseRecordedAudioClip(recording);
    return false;
  }

  if (recording.data == nullptr || recording.byteCount == 0 ||
      recording.sampleCount == 0 || recording.sampleRateHz == 0) {
    Serial.println("RECORD: empty");
    releaseRecordedAudioClip(recording);
    return false;
  }

  CloudQueuedJob job = {};
  job.type = CloudJobType::RecordedAudio;
  job.owner = aiOwned ? CloudJobOwner::Ai : CloudJobOwner::Manual;
  job.aiGeneration = aiCancelGeneration;
  job.recording = recording;
  recording = {};

  return queueCloudJob(job);
}

bool takeRecordedAudioCompletion(bool& processed, bool& wifiFailed) {
  bool available = false;

  portENTER_CRITICAL(&cloudJobMux);
  if (recordedAudioCompletionPending) {
    processed = recordedAudioProcessed;
    wifiFailed = recordedAudioWifiFailed;
    recordedAudioCompletionPending = false;
    recordedAudioProcessed = false;
    recordedAudioWifiFailed = false;
    recordedAudioCompletionOwner = CloudJobOwner::Manual;
    available = true;
  }
  portEXIT_CRITICAL(&cloudJobMux);

  return available;
}

}  // namespace decaflash::brain::api_client
