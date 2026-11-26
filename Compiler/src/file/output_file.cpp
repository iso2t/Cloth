// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/driver/Objects.rst

#include <backend/backend.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <system_error>

#include <diagnostics.hpp>
#include <file/atomic_file.hpp>
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


}

void emitFile (const CheckedProgram &program, backend::EmitKind kind, const fs::path &output) {
	checkDestination(program, output);
	const auto bytes = backend::emit(program, kind);
	try {
		publishFile(output, bytes);
	} catch (const std::system_error &error) {
		outputError(output, error.code());
	}
}

}
