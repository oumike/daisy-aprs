#pragma once

#include "runtime_config.h"

typedef void (*WebConfigSaveCb)();

bool webConfigBegin(RuntimeConfig* cfg, WebConfigSaveCb onSave);
void webConfigEnd();
void webConfigLoop();
bool webConfigRunning();
bool webConfigIsApMode();
const char* webConfigIP();
const char* webConfigSsid();
