// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/internals/Backend.rst

#include <cloth/array.h>
#include <cloth/object.h>
#include <cloth/text.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <limits.h>
#include <wchar.h>
#include <windows.h> // IWYU pragma: keep
// These API declarations use the Win32 types above.
#include <stringapiset.h>
#include <winnls.h>
#endif

// The emitter maps these results to the checked FileError case tags.
enum FileResult { Ok, InvalidPath, NotFound, PermissionDenied, OpenFailed, AlreadyOpen, NotOpen, ReadFailed };

struct Request {
	const ClothText *path;
	ClothArray      *bytes;
	size_t           limit;
	enum FileResult  result;
};

static void closeFile (void *file) {
	fclose(file);
}

static void openFile (void **file, void *argument) {
	struct Request *request = argument;
	const ClothText path    = *request->path;
	if (*file) {
		request->result = AlreadyOpen;
		return;
	}
	if (!path.length || memchr(path.data, 0, path.length)) {
		request->result = InvalidPath;
		return;
	}
#ifdef _WIN32
	if (path.length > INT_MAX) {
		request->result = InvalidPath;
		return;
	}
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)path.data, (int)path.length, NULL, 0);
	if (!length || (size_t)length >= SIZE_MAX / sizeof(wchar_t)) {
		request->result = InvalidPath;
		return;
	}
	wchar_t *name = calloc((size_t)length + 1, sizeof(*name));
	if (!name) {
		request->result = OpenFailed;
		return;
	}
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)path.data, (int)path.length, name, length);
	*file = _wfopen(name, L"rb");
#else
	char *name = malloc(path.length + 1);
	if (!name) {
		request->result = OpenFailed;
		return;
	}
	memcpy(name, path.data, path.length);
	name[path.length] = 0;
	*file             = fopen(name, "rb");
#endif
	const int error = errno;
	free(name);
	// POSIX supplies these constants in <errno.h>. glibc defines them in a private kernel header.
	// NOLINTNEXTLINE(misc-include-cleaner)
	if (!*file) request->result = error == ENOENT || error == ENOTDIR ? NotFound : error == EACCES || error == EPERM ? PermissionDenied : OpenFailed;
}

static void readFile (void **file, void *argument) {
	struct Request *request = argument;
	if (!*file) {
		request->result = NotOpen;
		return;
	}
	unsigned char buffer[8192];
	ClothArray   *bytes = request->bytes;
	while (bytes->length < request->limit) {
		const size_t left   = request->limit - bytes->length;
		const size_t wanted = left < sizeof(buffer) ? left : sizeof(buffer);
		const size_t size   = fread(buffer, 1, wanted, *file);
		if (size) {
			clothRtArrayReserve(bytes, bytes->length + size, 1, 1);
			memcpy(bytes->data + bytes->length, buffer, size);
			bytes->length += size;
		}
		if (size < wanted) {
			if (ferror(*file)) {
				clothRtArrayRelease(bytes);
				request->result = ReadFailed;
			}
			break;
		}
	}
}

int32_t clothRtFileOpen (const uint64_t handle, const ClothText *path) {
	struct Request request = { .path = path };
	clothRtObjectResource(handle, openFile, &request, closeFile);
	return request.result;
}

int32_t clothRtFileRead (const uint64_t handle, const size_t limit, ClothArray *bytes) {
	struct Request request = { .bytes = bytes, .limit = limit };
	clothRtObjectResource(handle, readFile, &request, closeFile);
	return request.result;
}
