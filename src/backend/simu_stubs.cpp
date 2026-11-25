// edgetx-lint: host hooks the EdgeTX simu drivers expect from their embedding
// program (EdgeTX's gtests-radio defines the same two in tests/gtests.cpp).
#include <cstdint>

uint16_t simu_get_analog(uint8_t) { return 0; }
void simuQueueAudio(const uint8_t*, uint32_t) {}
