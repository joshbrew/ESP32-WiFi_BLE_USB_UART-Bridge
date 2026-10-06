#pragma once
#include <cstddef>
#include <vector>
#include <cstring>
inline std::vector<std::vector<unsigned char>> testDatagrams;
struct WiFiUDP {
  void stop() {}
  bool begin(unsigned) { return true; }
  int parsePacket() { return testDatagrams.empty()?0:static_cast<int>(testDatagrams.front().size()); }
  void clear() { if(!testDatagrams.empty())testDatagrams.erase(testDatagrams.begin()); }
  int read(unsigned char *out,size_t size) { if(testDatagrams.empty())return 0; size_t n=std::min(size,testDatagrams.front().size());memcpy(out,testDatagrams.front().data(),n);clear();return static_cast<int>(n); }
};
