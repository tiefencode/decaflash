#pragma once

// Copy this file to `include/cloud_config.h` and fill in local worker settings.
// The real `cloud_config.h` must stay out of git.

namespace decaflash::secrets {

// Set this to the full Cloudflare Worker chattie endpoint.
static constexpr char kCloudChattieUrl[] = "https://example.workers.dev/api/chattie";

// Use the same shared secret value that the worker stores as BRAIN_SHARED_SECRET.
static constexpr char kBrainSharedSecret[] = "replace-with-brain-shared-secret";

}  // namespace decaflash::secrets
