#pragma once
#include "FreeRTOS.h"
#include <cstring>
#include <vector>
struct TestQueue { std::vector<uint8_t> bytes; bool full=false; };
using QueueHandle_t=TestQueue*;
inline QueueHandle_t xQueueCreate(unsigned,size_t size) { return new TestQueue{std::vector<uint8_t>(size),false}; }
inline int xQueueOverwrite(QueueHandle_t q,const void *data) { memcpy(q->bytes.data(),data,q->bytes.size());q->full=true;return pdPASS; }
inline int xQueueReceive(QueueHandle_t q,void *data,unsigned) { if(!q->full)return 0; memcpy(data,q->bytes.data(),q->bytes.size());q->full=false;return pdPASS; }
inline void xQueueReset(QueueHandle_t q) { q->full=false; }
