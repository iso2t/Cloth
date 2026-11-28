// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cloth/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

// After Cloth main returns, a C caller can reuse the same address and length for different UTF-8.
static void checkBorrowedText (void) {
	unsigned char   bytes[] = { 0xc3, 0xa9, 'x' };
	const ClothText view    = { .data = bytes, .length = sizeof(bytes) };
	if (clothRtTextAt(&view, 0) != 0xe9) abort();
	memcpy(bytes, "x\xc3\xa9", sizeof(bytes));
	if (clothRtTextAt(&view, 1) != 0xe9) abort();
}

utf8proc_ssize_t countCharAtDecode (const utf8proc_uint8_t *bytes, utf8proc_ssize_t length, utf8proc_int32_t *scalar) {
	static size_t calls;
	if (!calls && getenv("CLOTH_TEST_BORROW_AFTER_RETURN") && atexit(checkBorrowedText)) abort();
	// Allow one decode per scalar in the 2,048-character walk plus the short lookup and substring checks.
	if (++calls > 2048 + 32) {
		fputs("charAt rescanned the UTF-8 prefix\n", stderr);
		exit(1);
	}
	return utf8proc_iterate(bytes, length, scalar);
}
