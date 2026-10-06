#pragma once
constexpr int WL_CONNECTED=3;
constexpr int WIFI_AP=2;
struct TestWiFi { int statusValue=0, modeValue=0; int status() const { return statusValue; } int getMode() const{return modeValue;} int localIP() const{return 1;} int softAPIP() const{return 2;} };
inline TestWiFi WiFi;
