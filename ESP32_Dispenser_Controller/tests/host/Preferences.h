#pragma once
#include "Arduino.h"
#include <map>
#include <vector>
class Preferences {
  std::string space;
 public:
  inline static std::map<std::string,std::map<std::string,std::vector<uint8_t>>> records;
  bool begin(const char *name,bool=false) { space=name; return true; }
  void end() {}
  size_t getBytesLength(const char *key) { return records[space][key].size(); }
  size_t getBytes(const char *key,void *dest,size_t size) { auto &v=records[space][key]; size_t n=std::min(size,v.size()); if(n)memcpy(dest,v.data(),n); return n; }
  size_t putBytes(const char *key,const void *data,size_t size) { const auto *p=static_cast<const uint8_t*>(data); records[space][key]={p,p+size}; return size; }
  bool isKey(const char *key) { return records[space].count(key)!=0; }
  bool remove(const char *key) { return records[space].erase(key)!=0; }
  template<class T> T getNumber(const char *key,T fallback) { auto &v=records[space][key];T n=fallback;if(v.size()==sizeof(n))memcpy(&n,v.data(),sizeof(n));return n; }
  bool getBool(const char *key,bool fallback=false){return getNumber(key,fallback);}
  int getInt(const char *key,int fallback=0){return getNumber(key,fallback);}
  uint32_t getUInt(const char *key,uint32_t fallback=0){return getNumber(key,fallback);}
  uint8_t getUChar(const char *key,uint8_t fallback=0){return getNumber(key,fallback);}
  size_t putBool(const char *key,bool n){return putBytes(key,&n,sizeof(n));}
  size_t putInt(const char *key,int n){return putBytes(key,&n,sizeof(n));}
  size_t putUInt(const char *key,uint32_t n){return putBytes(key,&n,sizeof(n));}
  size_t putUChar(const char *key,uint8_t n){return putBytes(key,&n,sizeof(n));}
  size_t putString(const char *key,const char *s) { putBytes(key,s,strlen(s)+1); return strlen(s); }
  String getString(const char *key,const char *fallback="") { auto &v=records[space][key]; return v.empty()?fallback:reinterpret_cast<const char*>(v.data()); }
};
