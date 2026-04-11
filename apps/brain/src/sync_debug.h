#pragma once

namespace decaflash::brain::sync_debug {

bool autoLogEnabled();
void setAutoLogEnabled(bool enabled);
void requestStatusPrint();
bool consumeStatusPrintRequested();

}  // namespace decaflash::brain::sync_debug
