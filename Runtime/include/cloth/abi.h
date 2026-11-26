// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#define CLOTH_C_NAME(name)              cloth_##name
#define CLOTH_ABI_STRINGIFY_IMPL(value) #value
#define CLOTH_ABI_STRINGIFY(value)      CLOTH_ABI_STRINGIFY_IMPL(value)

// C clients and generated objects use cloth_rt_* symbols. Both C spellings below name
// the same function. An explicit assembler label includes the target's C symbol prefix.
#if defined(__GNUC__) || defined(__clang__)
#define CLOTH_RT_API(result, name, symbol, ...) \
	result cloth_rt_##symbol(__VA_ARGS__);  \
	result name(__VA_ARGS__) __asm__(CLOTH_ABI_STRINGIFY(__USER_LABEL_PREFIX__) "cloth_rt_" #symbol)
#else
// Other C toolchains can consume the packaged runtime through its original public names.
#define CLOTH_RT_API(result, name, symbol, ...) result cloth_rt_##symbol(__VA_ARGS__)
#endif
