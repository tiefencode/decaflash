#pragma once

// Copy this file to `include/relay_config.h` and fill in local relay settings.
// The real `relay_config.h` must stay out of git.

namespace decaflash::secrets {

static constexpr char kRelayTextUrl[] = "https://example.com/api/text";
static constexpr char kRelayBearerToken[] = "";

}  // namespace decaflash::secrets
