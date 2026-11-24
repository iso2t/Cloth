// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <backend/backend.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <span>
#include <system_error>
#include <wchar.h>

#include <diagnostics.hpp>
#include <file/output_file.hpp>
#include <file/source_file.hpp>
#include <semantic/checked_program.hpp>
#include <semantic/symbol_table.hpp>

namespace cloth {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void outputError (const fs::path &output, const std::error_code &error) {
	throw cde::ClothDiagnostic(cde::DiagnosticCode::OutputIo, std::format("Could not write output file '{}' ({})", pathUtf8(output), error.message()));
}

void checkDestination (const CheckedProgram &program, const fs::path &output) {
	std::error_code       ec;
	const fs::file_status fs_status = fs::status(output, ec);
	if (fs_status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory)) return;
	if (ec) outputError(output, ec);
	if (!fs::is_regular_file(fs_status))
		throw cde::ClothDiagnostic(cde::DiagnosticCode::OutputIo, std::format("Expected a regular file for output '{}'", pathUtf8(output)));

	for (const std::unique_ptr<const SymbolTable> &st_unit : program.units) {
		const SourceFile *sf_source = st_unit->getSourceUnit().getSource();
		if (!sf_source) continue;
		if (fs::equivalent(output, sf_source->getPath(), ec))
			throw cde::ClothDiagnostic(cde::DiagnosticCode::OutputOverwritesSource, std::format("Output '{}' would overwrite source file '{}'",
			                                                                                    pathUtf8(output), pathUtf8(sf_source->getPath())));
		if (ec) outputError(output, ec);
	}
}

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
		if (!fp_temp && errno != EEXIST) outputError(output, { errno, std::generic_category() });
	} while (!fp_temp);

	std::error_code ec;
	if (std::fwrite(bytes.data(), 1, bytes.size(), fp_temp) != bytes.size()) ec = { errno ? errno : EIO, std::generic_category() };
	if (std::fclose(fp_temp) != 0 && !ec) ec = { errno ? errno : EIO, std::generic_category() };
	if (!ec) fs::rename(p_temp, output, ec);
	if (ec) {
		std::error_code ec_cleanup;
		fs::remove(p_temp, ec_cleanup);
		outputError(output, ec);
	}
}

}

void emitFile (const CheckedProgram &program, backend::EmitKind kind, const fs::path &output) {
	checkDestination(program, output);
	publishFile(output, backend::emit(program, kind));
}

}
