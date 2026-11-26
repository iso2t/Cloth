// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/expressions/Builtins.rst

#include <cloth/array.h>
#include <cloth/text.h>
#include <runtime.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

struct StringEntry {
	struct StringEntry *next;
	size_t              length;
	unsigned char       data[];
};
static struct StringEntry *strings[1021];
static atomic_flag         stringsLock = ATOMIC_FLAG_INIT;
static int                 registered;

static void releaseStrings (void) {
	for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); ++i) {
		while (strings[i]) {
			struct StringEntry *entry = strings[i];
			strings[i]                = entry->next;
			free(entry);
		}
	}
}

static ClothText intern (const unsigned char *data, const size_t length) {
	if (!length) return (ClothText){ 0 };
	if (length > SIZE_MAX - sizeof(struct StringEntry)) clothRtError("text exceeds addressable memory");
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < length; ++i) hash = (hash ^ data[i]) * 16777619u;
	struct StringEntry **bucket = &strings[hash % (sizeof(strings) / sizeof(*strings))];
	while (atomic_flag_test_and_set_explicit(&stringsLock, memory_order_acquire)) {
	}
	struct StringEntry *entry = *bucket;
	while (entry && (entry->length != length || memcmp(entry->data, data, length))) entry = entry->next;
	if (!entry) {
		entry = malloc(sizeof(*entry) + length);
		if (entry) {
			entry->length = length;
			memcpy(entry->data, data, length);
			entry->next = *bucket;
			*bucket     = entry;
		}
		if (!registered) {
			if (atexit(releaseStrings)) clothRtError("unable to register text cleanup");
			registered = 1;
		}
	}
	atomic_flag_clear_explicit(&stringsLock, memory_order_release);
	if (!entry) clothRtError("unable to allocate text");
	return (ClothText){ .data = entry->data, .length = length };
}

static size_t decode (const ClothText value, const size_t at, int32_t *scalar) {
	if (at >= value.length) return 0;
	const size_t           left   = value.length - at;
	const utf8proc_ssize_t length = utf8proc_iterate(value.data + at, left < 4 ? (utf8proc_ssize_t)left : 4, scalar);
	return length > 0 ? (size_t)length : 0;
}

static size_t advance (const ClothText value, const size_t at) {
	int32_t      scalar = 0;
	const size_t width  = decode(value, at, &scalar);
	if (!width) clothRtError("invalid string index");
	return at + width;
}

static void append (ClothArray *bytes, const unsigned char *data, const size_t length) {
	if (!length) return;
	if (length > SIZE_MAX - bytes->length) clothRtError("text exceeds addressable memory");
	clothRtArrayReserve(bytes, bytes->length + length, 1, 1);
	memcpy(bytes->data + bytes->length, data, length);
	bytes->length += length;
}

static ClothText finish (ClothArray *bytes) {
	const ClothText value = intern(bytes->data, bytes->length);
	clothRtArrayRelease(bytes);
	return value;
}

static ClothText slice (const ClothText value, const size_t begin, const size_t end) {
	return begin == end ? (ClothText){ 0 } : (ClothText){ .data = value.data + begin, .length = end - begin };
}

static int matches (const ClothText value, const size_t at, const ClothText search) {
	return at <= value.length && search.length <= value.length - at && (!search.length || !memcmp(value.data + at, search.data, search.length));
}

uint8_t clothRtDecodeUtf8 (ClothText *result, const ClothArray *bytes) {
	const ClothText value = { .data = bytes->data, .length = bytes->length };
	for (size_t at = 0; at < value.length;) {
		int32_t      scalar = 0;
		const size_t width  = decode(value, at, &scalar);
		if (!width) return 0;
		at += width;
	}
	*result = intern(value.data, value.length);
	return 1;
}

void clothRtTextJoin (ClothText *result, const ClothText *parts, const size_t count) {
	ClothArray bytes = { 0 };
	for (size_t i = 0; i < count; ++i) append(&bytes, parts[i].data, parts[i].length);
	*result = finish(&bytes);
}

void clothRtTextWrite (const ClothText *value, const uint8_t newline) {
	if ((value->length && fwrite(value->data, 1, value->length, stdout) != value->length) || (newline && fputc('\n', stdout) == EOF) ||
	    fflush(stdout) == EOF)
		clothRtError("standard output write failed");
}

uint8_t clothRtTextInput (ClothText *result, const ClothText *prompt) {
	clothRtTextWrite(prompt, 0);
	ClothArray line = { 0 };
	int        c    = 0;
	while ((c = fgetc(stdin)) != EOF && c != '\n') {
		const unsigned char byte = (unsigned char)c;
		append(&line, &byte, 1);
	}
	if ((c == EOF && !line.length) || ferror(stdin)) {
		clothRtArrayRelease(&line);
		return 0;
	}
	if (c == '\n' && line.length && line.data[line.length - 1] == '\r') --line.length;
	const uint8_t valid = clothRtDecodeUtf8(result, &line);
	clothRtArrayRelease(&line);
	return valid;
}

void clothRtTextInteger (ClothText *out, const unsigned char *bits, const uint32_t width, const uint8_t isSigned, const int32_t radix) {
	if (radix < 2 || radix > 36) clothRtError("integer radix must be between 2 and 36");
	const size_t   bytes     = (width + 7u) / 8u;
	unsigned char *magnitude = malloc(bytes), *digits = malloc((size_t)width + 1);
	if (!magnitude || !digits) clothRtError("unable to format integer");
	const unsigned negative = isSigned && bits[(width - 1) / 8] & (1u << ((width - 1) % 8));
	unsigned       carry    = negative;
	for (size_t i = 0; i < bytes; ++i) {
		const unsigned byte = (negative ? bits[i] ^ 255u : bits[i]) + carry;
		magnitude[i]        = (unsigned char)byte;
		carry               = byte >> 8;
	}
	if (width % 8) magnitude[bytes - 1] &= (1u << (width % 8)) - 1;
	size_t used = bytes, pos = (size_t)width + 1;
	do {
		unsigned rem = 0;
		for (size_t i = used; i; --i) {
			const unsigned current = (rem << 8) | magnitude[i - 1];
			magnitude[i - 1]       = (unsigned char)(current / (unsigned)radix);
			rem                    = current % (unsigned)radix;
		}
		digits[--pos] = (unsigned char)"0123456789abcdefghijklmnopqrstuvwxyz"[rem];
		while (used && !magnitude[used - 1]) --used;
	} while (used);
	if (negative) digits[--pos] = '-';
	*out = intern(digits + pos, (size_t)width + 1 - pos);
	free(digits);
	free(magnitude);
}

void clothRtTextFloat (ClothText *out, const double value, const int32_t width) {
	char      buffer[64];
	const int size = snprintf(buffer, sizeof(buffer), "%.*g", width == 64 ? 17 : width == 32 ? 9 : 5, value);
	if (size < 0 || (size_t)size >= sizeof(buffer)) clothRtError("unable to format float");
	*out = intern((const unsigned char *)buffer, (size_t)size);
}

void clothRtTextChar (ClothText *out, const int32_t scalar) {
	unsigned char bytes[4];
	*out = intern(bytes, (size_t)utf8proc_encode_char(scalar, bytes));
}

uint8_t clothRtCharTest (const int32_t scalar, const int32_t test) {
	const utf8proc_category_t category = utf8proc_category(scalar);
	const int                 letter   = category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO;
	switch (test) {
	case ClothWhitespace:
		return (scalar >= 9 && scalar <= 13) || scalar == 0x85 || (category >= UTF8PROC_CATEGORY_ZS && category <= UTF8PROC_CATEGORY_ZP);
	case ClothLetter:
		return letter;
	case ClothDigit:
		return category == UTF8PROC_CATEGORY_ND;
	case ClothLetterOrDigit:
		return letter || category == UTF8PROC_CATEGORY_ND;
	case ClothUpper:
		return utf8proc_isupper(scalar);
	case ClothLower:
		return utf8proc_islower(scalar);
	default:
		clothRtError("unknown character query");
	}
}

int32_t clothRtCharCase (const int32_t value, const uint8_t upper) {
	return upper ? utf8proc_toupper(value) : utf8proc_tolower(value);
}

static void appendScalar (ClothArray *result, int32_t scalar) {
	unsigned char bytes[4];
	const size_t  width = (size_t)utf8proc_encode_char(scalar, bytes);
	append(result, bytes, width);
}

void clothRtTextFromChars (ClothText *out, const int32_t *scalars, const size_t count) {
	ClothArray result = { 0 };
	for (size_t i = 0; i < count; ++i) appendScalar(&result, scalars[i]);
	*out = finish(&result);
}

void clothRtTextCase (ClothText *out, const ClothText *value, const uint8_t upper) {
	ClothArray result = { 0 };
	for (size_t i = 0; i < value->length;) {
		int32_t scalar = 0;
		i += decode(*value, i, &scalar);
		appendScalar(&result, clothRtCharCase(scalar, upper));
	}
	*out = finish(&result);
}

static size_t offset (const ClothText value, size_t index) {
	size_t at = 0;
	while (index--) at = advance(value, at);
	return at;
}

int32_t clothRtTextAt (const ClothText *value, const size_t index) {
	int32_t scalar = 0;
	if (!decode(*value, offset(*value, index), &scalar)) clothRtError("string index out of bounds");
	return scalar;
}

void clothRtTextSubstring (ClothText *out, const ClothText *value, const size_t start, const size_t end) {
	if (end < start) clothRtError("invalid substring bounds");
	*out = slice(*value, offset(*value, start), offset(*value, end));
}

size_t clothRtTextFind (const ClothText *value, const ClothText *search, const uint8_t last) {
	size_t found = SIZE_MAX;
	for (size_t i = 0, index = 0;; ++index) {
		if (matches(*value, i, *search)) {
			found = index;
			if (!last) break;
		}
		if (i == value->length) break;
		i = advance(*value, i);
	}
	return found;
}

uint8_t clothRtTextMatches (const ClothText *value, const ClothText *search, const int32_t end) {
	if (end == ClothBoth) return value->length == search->length && matches(*value, 0, *search);
	return search->length <= value->length && matches(*value, end == ClothStart ? 0 : value->length - search->length, *search);
}

void clothRtTextAffix (ClothText *out, const ClothText *value, const ClothText *search, const int32_t end) {
	*out = !clothRtTextMatches(value, search, end) ? *value :
	       end == ClothStart                       ? slice(*value, search->length, value->length) :
	                                                 slice(*value, 0, value->length - search->length);
}

void clothRtTextTrim (ClothText *out, const ClothText *value, const int32_t ends) {
	size_t first = value->length, last = 0;
	for (size_t i = 0; i < value->length;) {
		int32_t      scalar = 0;
		const size_t start  = i;
		i += decode(*value, i, &scalar);
		if (!clothRtCharTest(scalar, ClothWhitespace)) {
			if (first == value->length) first = start;
			last = i;
		}
	}
	if (first == value->length) {
		*out = (ClothText){ 0 };
		return;
	}
	*out = slice(*value, ends & ClothStart ? first : 0, ends & ClothEnd ? last : value->length);
}

void clothRtTextChars (ClothArray *out, const ClothText *value) {
	for (size_t i = 0; i < value->length;) {
		int32_t scalar = 0;
		i += decode(*value, i, &scalar);
		clothRtArrayAppend(out, &scalar, sizeof(scalar), _Alignof(int32_t));
	}
}

void clothRtTextSplit (ClothArray *out, const ClothText *value, const ClothText *separator) {
	size_t start = 0;
	for (size_t i = 0;;) {
		if (matches(*value, i, *separator)) {
			const ClothText part = slice(*value, start, i);
			clothRtArrayAppend(out, &part, sizeof(part), _Alignof(ClothText));
			start = i + separator->length;
			if (separator->length) {
				i = start;
				continue;
			}
		}
		if (i == value->length) break;
		i = advance(*value, i);
	}
	const ClothText part = slice(*value, start, value->length);
	clothRtArrayAppend(out, &part, sizeof(part), _Alignof(ClothText));
}

void clothRtTextReplace (ClothText *out, const ClothText *value, const ClothText *search, const ClothText *replacement, const uint8_t first) {
	ClothArray result = { 0 };
	size_t     start  = 0;
	for (size_t i = 0;;) {
		if (matches(*value, i, *search)) {
			const ClothText part = slice(*value, start, i);
			append(&result, part.data, part.length);
			append(&result, replacement->data, replacement->length);
			start = i + search->length;
			if (first) break;
			if (search->length) {
				i = start;
				continue;
			}
		}
		if (i == value->length) break;
		i = advance(*value, i);
	}
	const ClothText tail = slice(*value, start, value->length);
	append(&result, tail.data, tail.length);
	*out = finish(&result);
}
