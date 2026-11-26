// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <random>
#include <span>
#include <system_error>
#ifdef _WIN32
#include <stdio.h> // _wfopen
#endif

#include <file/atomic_file.hpp>
#include <file/source_file.hpp>

namespace cloth {
namespace fs = std::filesystem;

void publishFile (const fs::path &output, const std::span<const std::byte> bytes) {
	// Use a short sibling name distinct from the destination, even when its filename is at the filesystem limit.
	const char *s_prefix = output.filename().native().starts_with('.') ? "cloth-" : ".cloth-";
	unsigned    u_nonce  = std::random_device{}();
	fs::path    p_temp;
	std::FILE  *fp_temp = nullptr;
	do {
		p_temp = output.parent_path() / std::format("{}{:08x}.tmp", s_prefix, u_nonce++);
#ifdef _WIN32
		fp_temp = _wfopen(p_temp.c_str(), L"wbx");
#else
		fp_temp = std::fopen(p_temp.c_str(), "wbx");
#endif
		if (!fp_temp && errno != EEXIST) throw std::system_error(errno, std::generic_category(), pathUtf8(output));
	} while (!fp_temp);

	std::error_code ec;
	if (std::fwrite(bytes.data(), 1, bytes.size(), fp_temp) != bytes.size()) ec = { errno ? errno : EIO, std::generic_category() };
	if (std::fclose(fp_temp) != 0 && !ec) ec = { errno ? errno : EIO, std::generic_category() };
	if (!ec) fs::rename(p_temp, output, ec);
	if (ec) {
		std::error_code cleanup;
		fs::remove(p_temp, cleanup);
		throw std::system_error(ec, pathUtf8(output));
	}
}
}
