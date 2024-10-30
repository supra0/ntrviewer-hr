#ifndef MAIN_H
#define MAIN_H

#include "const.h"

#ifdef _WIN32
#include <combaseapi.h>
#define RO_INIT() CoInitializeEx(NULL, COINIT_MULTITHREADED)
#define RO_UNINIT() CoUninitialize()
#else
#define RO_INIT()
#define RO_UNINIT()
#endif

#endif
