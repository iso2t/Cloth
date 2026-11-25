// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
// The shell declarations use the Win32 types above.
#include <shellapi.h>
#endif

struct cloth_string {
	const char *data;
	size_t      length;
};

_Static_assert(offsetof(struct cloth_string, length) == sizeof(void *) && sizeof(struct cloth_string) == 2 * sizeof(size_t),
               "Cloth strings contain a pointer and a native byte count");

_Noreturn void cloth_rt_array_allocation_failed (void);

static _Noreturn void argument_error (void) {
	fputs("Cloth runtime error: invalid Unicode in process arguments\n", stderr);
	exit(1);
}

#ifndef _WIN32
static void validate_utf8 (const unsigned char *text) {
	while (*text) {
		const unsigned char lead = *text++;
		if (lead < 0x80) continue;
		if (lead < 0xc2 || lead > 0xf4) argument_error();
		const unsigned count = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
		// Reject overlong forms, surrogate code points and values above U+10FFFF.
		if ((lead == 0xe0 && *text < 0xa0) || (lead == 0xed && *text >= 0xa0) || (lead == 0xf0 && *text < 0x90) || (lead == 0xf4 && *text >= 0x90))
			argument_error();
		for (unsigned index = 0; index < count; ++index) {
			if ((*text & 0xc0) != 0x80) argument_error();
			++text;
		}
	}
}
#endif

struct cloth_string *cloth_rt_process_arguments (int argc, char **argv, size_t *count) {
#ifdef _WIN32
	(void)argv;
	wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!wide) cloth_rt_array_allocation_failed();
#endif
	*count                         = argc > 1 ? (size_t)(argc - 1) : 0;
	struct cloth_string *arguments = *count ? calloc(*count, sizeof(*arguments)) : NULL;
	if (*count && !arguments) cloth_rt_array_allocation_failed();
	for (size_t index = 0; index < *count; ++index) {
#ifdef _WIN32
		const wchar_t *source = wide[index + 1];
		const int      bytes  = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, NULL, 0, NULL, NULL);
		if (!bytes) argument_error();
		char *text = malloc((size_t)bytes);
		if (!text) cloth_rt_array_allocation_failed();
		if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, text, bytes, NULL, NULL)) argument_error();
		arguments[index] = (struct cloth_string){ text, (size_t)(bytes - 1) };
#else
		const char *text = argv[index + 1];
		validate_utf8((const unsigned char *)text);
		arguments[index] = (struct cloth_string){ text, strlen(text) };
#endif
	}
#ifdef _WIN32
	LocalFree(wide);
#endif
	return arguments;
}

void cloth_rt_release_arguments (struct cloth_string *arguments, size_t count) {
#ifdef _WIN32
	for (size_t index = 0; index < count; ++index) free((void *)arguments[index].data);
#else
	(void)count;
#endif
	free(arguments);
}
