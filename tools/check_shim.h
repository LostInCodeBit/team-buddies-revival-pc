// Only used for syntax checks with a non-Windows clang; the real build uses MSVC.
#pragma once
#include <cstdio>
#include <strings.h>
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _strnicmp strncasecmp
#define _stricmp strcasecmp
