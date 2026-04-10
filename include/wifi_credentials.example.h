#pragma once

// Copy this file to `include/wifi_credentials.h` and fill in local credentials.
// The real `wifi_credentials.h` must stay out of git.
//
// Preferred format:
// - list one or more WLANs
// - keep the most preferred SSID first
// - the brain scans for visible configured SSIDs and tries them in this order
#define DECAFLASH_WIFI_CREDENTIALS_LIST(X) \
  X("Home 2.4 GHz", "replace-me") \
  X("Phone Hotspot", "replace-me")
