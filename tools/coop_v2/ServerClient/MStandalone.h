#pragma once

#include "SteamWorksSDK/include/steamnetworkingsockets.h"
#include "SteamWorksSDK/include/isteamnetworkingutils.h"

ISteamNetworkingSockets* AcquireStandaloneSockets();
ISteamNetworkingUtils* GetStandaloneNetworkingUtils();
void ShutdownStandaloneSockets();
