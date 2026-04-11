#include "sync_debug.h"

namespace decaflash::brain::sync_debug {

namespace {

bool autoLogEnabledState = false;
bool statusPrintRequested = false;

}  // namespace

bool autoLogEnabled() {
  return autoLogEnabledState;
}

void setAutoLogEnabled(bool enabled) {
  autoLogEnabledState = enabled;
}

void requestStatusPrint() {
  statusPrintRequested = true;
}

bool consumeStatusPrintRequested() {
  const bool requested = statusPrintRequested;
  statusPrintRequested = false;
  return requested;
}

}  // namespace decaflash::brain::sync_debug
