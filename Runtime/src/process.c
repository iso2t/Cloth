// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst
// See Documentation/language/expressions/Builtins.rst

#include <runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

_Noreturn void clothRtExit (int32_t status) {
	exit(status);
}

static _Noreturn void outputError (void) {
	fputs("Cloth runtime error: standard output write failed\n", stderr);
	exit(1);
}

_Noreturn void clothRtExitMessage (int32_t status, const unsigned char *data, size_t length) {
	if (length != 0) {
#ifdef _WIN32
		// Flush preceding text in its original mode. The message is a byte span and may contain literal LF bytes.
		if (fflush(stdout) == EOF) outputError();
		const int originalMode = _setmode(_fileno(stdout), _O_BINARY);
		if (originalMode == -1) outputError();
#endif
		if (fwrite(data, 1, length, stdout) != length || fflush(stdout) == EOF) outputError();
#ifdef _WIN32
		if (_setmode(_fileno(stdout), originalMode) == -1) outputError();
#endif
	}
	clothRtExit(status);
}
