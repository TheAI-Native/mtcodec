#define main dcc_original_cli_main
#include "mtcodec_original.c"
#undef main
#include "dcc_bridge.h"
#define DCC_PREFIX dcc_ref
#include "dcc_bridge_impl.h"
