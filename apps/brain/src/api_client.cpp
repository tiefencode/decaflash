#include "api_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cstring>

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
static constexpr char kCloudChattieMonitorPrompt[] =
  "Schreibe genau einen sehr kurzen kreativen deutschen Text fuer eine 5x5-LED-Matrix. "
  "Nutze die Nutzereingabe als Inspiration. Keine Erklaerung. Keine Emojis. "
  "Maximal 8 Woerter.";
static constexpr char kCloudChattieSongPrompt[] =
  "Schreibe genau einen sehr kurzen kreativen deutschen Text fuer eine 5x5-LED-Matrix. "
  "Nutze nur Songtitel und Artist als Inspiration. Keine Lyrics. Keine Zitate. "
  "Keine Erklaerung. Keine Emojis. Maximal 8 Woerter.";
static constexpr char kCloudMultipartBoundary[] = "----decaflash-boundary-7e8db64a";

class MultipartWavStream final : public Stream {
 public:
  MultipartWavStream(const char* preamble,
                     size_t preambleLength,
                     const uint8_t* wavHeader,
                     size_t wavHeaderLength,
                     const int16_t* samples,
                     size_t sampleCount,
                     const char* footer,
                     size_t footerLength)
    : preamble_(reinterpret_cast<const uint8_t*>(preamble)),
      preambleLength_(preambleLength),
      wavHeader_(wavHeader),
      wavHeaderLength_(wavHeaderLength),
      pcmData_(reinterpret_cast<const uint8_t*>(samples)),
      pcmDataLength_(sampleCount * sizeof(int16_t)),
      footer_(reinterpret_cast<const uint8_t*>(footer)),
      footerLength_(footerLength),
      totalLength_(preambleLength_ + wavHeaderLength_ + pcmDataLength_ + footerLength_) {
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
      if (section.length == 0 || section.data == nullptr) {
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
    const Section section = sectionForOffset(absoluteOffset);
    if (section.data == nullptr || section.length == 0) {
      return -1;
    }

    return section.data[absoluteOffset - section.startOffset];
  }

  Section activeSection() const {
    return sectionForOffset(position_);
  }

  Section sectionForOffset(size_t absoluteOffset) const {
    if (absoluteOffset < preambleLength_) {
      return {preamble_, 0, preambleLength_};
    }

    const size_t wavOffset = preambleLength_;
    if (absoluteOffset < (wavOffset + wavHeaderLength_)) {
      return {wavHeader_, wavOffset, wavHeaderLength_};
    }

    const size_t pcmOffset = wavOffset + wavHeaderLength_;
    if (absoluteOffset < (pcmOffset + pcmDataLength_)) {
      return {pcmData_, pcmOffset, pcmDataLength_};
    }

    const size_t footerOffset = pcmOffset + pcmDataLength_;
    if (absoluteOffset < (footerOffset + footerLength_)) {
      return {footer_, footerOffset, footerLength_};
    }

    return {nullptr, totalLength_, 0};
  }

  const uint8_t* preamble_;
  size_t preambleLength_;
  const uint8_t* wavHeader_;
  size_t wavHeaderLength_;
  const uint8_t* pcmData_;
  size_t pcmDataLength_;
  const uint8_t* footer_;
  size_t footerLength_;
  size_t totalLength_;
  size_t position_ = 0;
};

bool fetchCloudChattieText(const char* input,
                           const char* instructions,
                           char* destination,
                           size_t capacity);

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

  Serial.println("api=abort reason=wifi_not_connected");
  return false;
}

bool appendCharacter(char* destination, size_t capacity, size_t& length, char value) {
  if ((length + 1U) >= capacity) {
    return false;
  }

  destination[length++] = value;
  destination[length] = '\0';
  return true;
}

void printResponseBodySnippet(const char* label, const String& body) {
  if (label == nullptr || body.isEmpty()) {
    return;
  }

  String snippet = body;
  snippet.replace('\r', ' ');
  snippet.replace('\n', ' ');
  if (snippet.length() > 180) {
    snippet.remove(180);
    snippet += "...";
  }

  Serial.printf("api=%s_body body=\"%s\"\n", label, snippet.c_str());
}

bool extractJsonStringField(const String& body,
                            const char* fieldName,
                            char* destination,
                            size_t capacity) {
  if (capacity == 0) {
    return false;
  }

  destination[0] = '\0';
  char fieldPattern[40] = {};
  snprintf(fieldPattern, sizeof(fieldPattern), "\"%s\"", fieldName);

  const char* keyStart = strstr(body.c_str(), fieldPattern);
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

bool extractJsonBoolField(const String& body, const char* fieldName, bool& value) {
  char fieldPattern[40] = {};
  snprintf(fieldPattern, sizeof(fieldPattern), "\"%s\"", fieldName);

  const char* keyStart = strstr(body.c_str(), fieldPattern);
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

void appendJsonEscaped(String& destination, const char* source) {
  if (source == nullptr) {
    return;
  }

  while (*source != '\0') {
    switch (*source) {
      case '\\':
        destination += "\\\\";
        break;

      case '"':
        destination += "\\\"";
        break;

      case '\n':
        destination += "\\n";
        break;

      case '\r':
        destination += "\\r";
        break;

      case '\t':
        destination += "\\t";
        break;

      default:
        destination += *source;
        break;
    }

    source++;
  }
}

bool beginSecureRequest(HTTPClient& http,
                        WiFiClientSecure& client,
                        const char* url,
                        const char* label) {
  client.setInsecure();
  if (!http.begin(client, url)) {
    Serial.printf("api=begin_failed endpoint=%s\n", label);
    return false;
  }

  http.addHeader("User-Agent", "decaflash-brain");
  http.addHeader("Accept", "application/json, text/plain");
  return true;
}

void addCloudAuthorizationHeader(HTTPClient& http) {
  String bearerValue = "Bearer ";
  bearerValue += decaflash::secrets::kBrainSharedSecret;
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
    Serial.println("api=cloud_config_invalid expected=kCloudChattieUrl_ends_with_/api/chattie");
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
                   const String& payload,
                   String& responseBody) {
  if (!beginSecureRequest(http, client, url, label)) {
    return false;
  }

  addCloudAuthorizationHeader(http);
  http.addHeader("Content-Type", "application/json");

  const int statusCode = http.POST(payload);
  if (statusCode <= 0) {
    Serial.printf("api=request_failed endpoint=%s err=%s\n",
                  label,
                  http.errorToString(statusCode).c_str());
    http.end();
    return false;
  }

  responseBody = http.getString();
  http.end();

  Serial.printf("api=%s status=%d bytes=%u\n",
                label,
                statusCode,
                static_cast<unsigned>(responseBody.length()));

  if (statusCode != HTTP_CODE_OK || responseBody.isEmpty()) {
    printResponseBodySnippet(label, responseBody);
    Serial.printf("api=%s_empty\n", label);
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
                     String& responseBody) {
  if (!beginSecureRequest(http, client, url, label)) {
    return false;
  }

  addCloudAuthorizationHeader(http);
  http.addHeader("Content-Type", contentType);

  const int statusCode = http.sendRequest("POST", &stream, contentLength);
  if (statusCode <= 0) {
    Serial.printf("api=request_failed endpoint=%s err=%s\n",
                  label,
                  http.errorToString(statusCode).c_str());
    http.end();
    return false;
  }

  responseBody = http.getString();
  http.end();

  Serial.printf("api=%s status=%d bytes=%u\n",
                label,
                statusCode,
                static_cast<unsigned>(responseBody.length()));

  if (statusCode != HTTP_CODE_OK || responseBody.isEmpty()) {
    printResponseBodySnippet(label, responseBody);
    Serial.printf("api=%s_empty\n", label);
    return false;
  }

  return true;
}

bool fetchCloudGeneratedSongText(const char* title,
                                 const char* artist,
                                 char* destination,
                                 size_t capacity) {
  if (capacity == 0 || title == nullptr || artist == nullptr) {
    return false;
  }

  String input = "Titel: ";
  input += title;
  input += "\nArtist: ";
  input += artist;

  return fetchCloudChattieText(input.c_str(), kCloudChattieSongPrompt, destination, capacity);
}

bool fetchCloudSongMetadataFromRecording(const int16_t* samples,
                                         size_t sampleCount,
                                         uint32_t sampleRateHz,
                                         char* title,
                                         size_t titleCapacity,
                                         char* artist,
                                         size_t artistCapacity) {
  if (samples == nullptr || sampleCount == 0 || sampleRateHz == 0 ||
      titleCapacity == 0 || artistCapacity == 0) {
    return false;
  }

  title[0] = '\0';
  artist[0] = '\0';

  char auddUrl[160] = {};
  if (!deriveCloudAuddUrl(auddUrl, sizeof(auddUrl))) {
    return false;
  }

  char preamble[192] = {};
  const int preambleLength = snprintf(
    preamble,
    sizeof(preamble),
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n"
    "Content-Type: audio/wav\r\n"
    "\r\n",
    kCloudMultipartBoundary
  );
  if (preambleLength <= 0 || static_cast<size_t>(preambleLength) >= sizeof(preamble)) {
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
    return false;
  }

  uint8_t wavHeader[44] = {};
  buildWavHeader(wavHeader, sampleCount, sampleRateHz, 1, 16);

  MultipartWavStream stream(
    preamble,
    static_cast<size_t>(preambleLength),
    wavHeader,
    sizeof(wavHeader),
    samples,
    sampleCount,
    footer,
    static_cast<size_t>(footerLength)
  );

  String contentType = "multipart/form-data; boundary=";
  contentType += kCloudMultipartBoundary;

  WiFiClientSecure client;
  HTTPClient http;
  String body;
  if (!postCloudStream(http,
                       client,
                       auddUrl,
                       "audd",
                       contentType.c_str(),
                       stream,
                       stream.totalLength(),
                       body)) {
    return false;
  }

  bool matched = false;
  if (!extractJsonBoolField(body, "matched", matched)) {
    Serial.println("api=audd_parse_failed expected=matched_bool");
    return false;
  }

  if (!matched) {
    Serial.println("api=audd_no_match");
    return false;
  }

  if (!extractJsonStringField(body, "title", title, titleCapacity) ||
      !extractJsonStringField(body, "artist", artist, artistCapacity)) {
    Serial.println("api=audd_parse_failed expected=title_and_artist");
    return false;
  }

  Serial.printf("api=audd_match artist=\"%s\" title=\"%s\"\n", artist, title);
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

  String payload = "{\"instructions\":\"";
  appendJsonEscaped(payload, instructions);
  payload += "\",\"input\":\"";
  appendJsonEscaped(payload, input);
  payload += "\"}";

  WiFiClientSecure client;
  HTTPClient http;
  String body;
  if (!postCloudJson(http, client, decaflash::secrets::kCloudChattieUrl, "chattie", payload, body)) {
    return false;
  }

  if (!extractJsonStringField(body, "text", destination, capacity)) {
    Serial.println("api=chattie_parse_failed expected=text");
    return false;
  }

  Serial.printf("api=chattie_text text=\"%s\"\n", destination);
  return true;
}

}  // namespace

bool fetchCloudChattieInputToTextDisplay(const char* input) {
  if (!cloudConfigured()) {
    Serial.println("api=cloud_missing_config file=include/cloud_config.h");
    Serial.println("api=cloud_expected keys=kCloudChattieUrl,kBrainSharedSecret");
    return false;
  }

  if (input == nullptr || input[0] == '\0') {
    Serial.println("api=chattie_missing_input");
    return false;
  }

  if (!ensureWifiConnected()) {
    return false;
  }

  char displayText[kCloudTextCapacity] = {};
  if (!fetchCloudChattieText(
        input, kCloudChattieMonitorPrompt, displayText, sizeof(displayText))) {
    return false;
  }

  return decaflash::brain::text_playback::start(displayText);
}

bool processRecordedAudioToTextDisplay(const int16_t* samples,
                                       size_t sampleCount,
                                       uint32_t sampleRateHz) {
  if (!cloudConfigured()) {
    Serial.println("api=cloud_missing_config file=include/cloud_config.h");
    Serial.println("api=cloud_expected keys=kCloudChattieUrl,kBrainSharedSecret");
    return false;
  }

  if (samples == nullptr || sampleCount == 0 || sampleRateHz == 0) {
    Serial.println("record=empty");
    return false;
  }

  if (!ensureWifiConnected()) {
    return false;
  }

  char title[kCloudTitleCapacity] = {};
  char artist[kCloudArtistCapacity] = {};
  if (!fetchCloudSongMetadataFromRecording(
        samples, sampleCount, sampleRateHz, title, sizeof(title), artist, sizeof(artist))) {
    return false;
  }

  char displayText[kCloudTextCapacity] = {};
  if (!fetchCloudGeneratedSongText(title, artist, displayText, sizeof(displayText))) {
    return false;
  }

  return decaflash::brain::text_playback::start(displayText);
}

}  // namespace decaflash::brain::api_client
