// edgetx-lint: single place that pulls in EdgeTX headers.
// Works for both the 2.10 tree (opentx.h) and 2.11+ (edgetx.h).
#pragma once

#if __has_include("edgetx.h")
#include "edgetx.h"
#else
#include "opentx.h"
#endif

#include "stamp.h"
#include "model_init.h"
#include "crc.h"
#include "simpgmspace.h"
#include "storage/sdcard_yaml.h"
#include "storage/yaml/yaml_bits.h"
#include "storage/yaml/yaml_datastructs.h"
#include "storage/yaml/yaml_node.h"
#include "storage/yaml/yaml_parser.h"
#include "storage/yaml/yaml_tree_walker.h"

#define ETX_VERSION_NUM (VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_REVISION)
