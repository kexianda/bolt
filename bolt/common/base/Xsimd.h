/*
 * Copyright (c) ByteDance Ltd. and/or its affiliates
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// xsimd only enables RVV when the compiler exposes both __riscv_vector and a
// fixed VLEN. Keep the batch API available on RISC-V CPUs without RVV by
// selecting xsimd's array-based emulated architecture. Include the config
// first so we can override its no-architecture decision before xsimd.hpp is
// parsed.
#include <xsimd/config/xsimd_config.hpp>
#if defined(BOLT_XSIMD_SCALAR_FALLBACK)
#undef XSIMD_NO_SUPPORTED_ARCHITECTURE
#undef XSIMD_WITH_EMULATED
#define XSIMD_WITH_EMULATED 1
#define XSIMD_DEFAULT_ARCH xsimd::emulated<128>
#endif
#include <xsimd/xsimd.hpp>
