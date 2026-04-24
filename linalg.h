#pragma once

// Umbrella header kept for back-compat: existing callers that
// `#include "linalg.h"` keep compiling after the vec / mat split.
// New code should include vec.h or mat.h directly.
#include "mat.h"
#include "vec.h"
