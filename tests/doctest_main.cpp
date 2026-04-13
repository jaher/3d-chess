// The single translation unit that pulls in the doctest implementation.
// Every other test file just includes "doctest.h" normally and adds
// TEST_CASE blocks — they don't define the IMPLEMENT macro.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
