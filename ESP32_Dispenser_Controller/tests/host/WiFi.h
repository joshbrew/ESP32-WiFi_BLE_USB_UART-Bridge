#pragma once
constexpr int WL_CONNECTED=3;
struct TestWiFi { int statusValue=0; int status() const { return statusValue; } };
inline TestWiFi WiFi;
