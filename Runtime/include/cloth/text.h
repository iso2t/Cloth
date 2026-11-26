// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "abi.h"
#include "array.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CLOTH_C_NAME(text) {
	const unsigned char *data;
	size_t               length;
} ClothText;

typedef enum CLOTH_C_NAME(character_test) { ClothWhitespace, ClothLetter, ClothDigit, ClothLetterOrDigit, ClothUpper, ClothLower } ClothCharacterTest;

typedef enum CLOTH_C_NAME(text_end) { ClothStart = 1, ClothEnd = 2, ClothBoth = 3 } ClothTextEnd;

#define CLOTH_WHITESPACE      ClothWhitespace
#define CLOTH_LETTER          ClothLetter
#define CLOTH_DIGIT           ClothDigit
#define CLOTH_LETTER_OR_DIGIT ClothLetterOrDigit
#define CLOTH_UPPER           ClothUpper
#define CLOTH_LOWER           ClothLower
#define CLOTH_START           ClothStart
#define CLOTH_END             ClothEnd
#define CLOTH_BOTH            ClothBoth

CLOTH_RT_API(uint8_t, clothRtDecodeUtf8, decode_utf8, ClothText *result, const ClothArray *bytes);
CLOTH_RT_API(void, clothRtTextJoin, text_join, ClothText *result, const ClothText *parts, size_t count);
CLOTH_RT_API(void, clothRtTextWrite, text_write, const ClothText *value, uint8_t newline);
CLOTH_RT_API(uint8_t, clothRtTextInput, text_input, ClothText *result, const ClothText *prompt);
CLOTH_RT_API(void, clothRtTextInteger, text_integer, ClothText *out, const unsigned char *bits, uint32_t width, uint8_t isSigned, int32_t radix);
CLOTH_RT_API(void, clothRtTextFloat, text_float, ClothText *out, double value, int32_t width);
CLOTH_RT_API(void, clothRtTextChar, text_char, ClothText *out, int32_t scalar);
CLOTH_RT_API(uint8_t, clothRtCharTest, char_test, int32_t scalar, int32_t test);
CLOTH_RT_API(int32_t, clothRtCharCase, char_case, int32_t value, uint8_t upper);
CLOTH_RT_API(void, clothRtTextCase, text_case, ClothText *out, const ClothText *value, uint8_t upper);
CLOTH_RT_API(int32_t, clothRtTextAt, text_at, const ClothText *value, size_t index);
CLOTH_RT_API(void, clothRtTextSubstring, text_substring, ClothText *out, const ClothText *value, size_t start, size_t end);
CLOTH_RT_API(size_t, clothRtTextFind, text_find, const ClothText *value, const ClothText *search, uint8_t last);
CLOTH_RT_API(uint8_t, clothRtTextMatches, text_matches, const ClothText *value, const ClothText *search, int32_t end);
CLOTH_RT_API(void, clothRtTextAffix, text_affix, ClothText *out, const ClothText *value, const ClothText *search, int32_t end);
CLOTH_RT_API(void, clothRtTextTrim, text_trim, ClothText *out, const ClothText *value, int32_t ends);
CLOTH_RT_API(void, clothRtTextChars, text_chars, ClothArray *out, const ClothText *value);
CLOTH_RT_API(void, clothRtTextSplit, text_split, ClothArray *out, const ClothText *value, const ClothText *separator);
CLOTH_RT_API(void, clothRtTextReplace, text_replace, ClothText *out, const ClothText *value, const ClothText *search, const ClothText *replacement,
             uint8_t first);

CLOTH_RT_API(int32_t, clothRtFileOpen, file_open, uint64_t handle, const ClothText *path);
CLOTH_RT_API(int32_t, clothRtFileRead, file_read, uint64_t handle, size_t limit, ClothArray *bytes);

#ifdef __cplusplus
}
#endif
