// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst
// See Documentation/language/expressions/Builtins.rst

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

_Noreturn void cloth_rt_exit (int32_t status) {
	exit(status);
}

static _Noreturn void output_error (void) {
	fputs("Cloth runtime error: standard output write failed\n", stderr);
	exit(1);
}

_Noreturn void cloth_rt_exit_message (int32_t status, const unsigned char *data, size_t length) {
	if (length != 0) {
#ifdef _WIN32
		// Flush preceding text in its original mode. The message is a byte span and may contain literal LF bytes.
		if (fflush(stdout) == EOF) output_error();
		const int original_mode = _setmode(_fileno(stdout), _O_BINARY);
		if (original_mode == -1) output_error();
#endif
		if (fwrite(data, 1, length, stdout) != length || fflush(stdout) == EOF) output_error();
#ifdef _WIN32
		if (_setmode(_fileno(stdout), original_mode) == -1) output_error();
#endif
	}
	cloth_rt_exit(status);
}
