#pragma once

namespace decaflash::brain::wifi_manager {

bool credentialsAvailable();
bool connect();
void disconnect();
bool isConnected();
void scan();
void printStatus();

}  // namespace decaflash::brain::wifi_manager
