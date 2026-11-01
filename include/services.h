#pragma once

#include "app_data.h"

void startNetworkServices();
void copyAppData(AppData &destination);
bool sendRadioCommand(const char *query);
// Switch the receiver between WEB (0) and SD (1) modes.
void setRadioMode(uint8_t mode);
// Ask the network task to re-download weather, rates and radio lists.
void requestDataRefresh();
// Ask the network task to re-download the yoRadio playlists only.
void requestRadioLists();

// LG NetCast TV (ROAP, port 8080), implemented in tv.cpp. The pairing key
// is entered once in the web configurator; sessions persist in NVS.
bool sendTvKey(int code);
bool sendTvChannel(unsigned number);
void tvPairShowKey();
void requestTvStatus();
void copyTvData(TvData &destination);
void requestRateHistory(uint8_t currency, uint8_t months);
void copyRateHistory(RateHistory &destination);
