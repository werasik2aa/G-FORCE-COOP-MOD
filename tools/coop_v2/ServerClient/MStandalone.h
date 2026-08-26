#pragma once

#include "SteamWorksSDK/include/steamnetworkingsockets.h"
#include "SteamWorksSDK/include/isteamnetworkingutils.h"

ISteamNetworkingSockets* AcquireStandaloneSockets();
ISteamNetworkingUtils* GetStandaloneNetworkingUtils();
// SteamNetworkingIPAddr::ParseString delegates through Steam API state in this
// SDK build.  The IP-only transport must not touch that state when Steam is off.
bool ParseStandaloneIPv4Address(const char* text, SteamNetworkingIPAddr& address);
void ShutdownStandaloneSockets();
