// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <exception>
#include <filesystem>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cde/cloth_diagnostic.hpp>
#include <cde/diagnostic_reporter.hpp>
#include <file/atomic_file.hpp>
#include <file/source_file.hpp>
#include <migration/semicolons.hpp>

namespace {
namespace fs = std::filesystem;
using cloth::migration::InputDialect;

constexpr std::string_view USAGE = "Usage: cloth-migrate-semicolons [--from-legacy] [--] PATH...\n"
                                   "Convert .co and .clib files or directories in place. Unknown-dialect inputs with conflicting parses are reported.\n";

void collect (const fs::path &path, std::set<fs::path> &files) {
	if (!fs::is_directory(path)) {
		files.insert(path);
		return;
	}
	for (auto it = fs::recursive_directory_iterator(path); it != fs::recursive_directory_iterator(); ++it) {
		if (it->is_directory() && it->path().filename() == ".git") it.disable_recursion_pending();
		if (!it->is_regular_file()) continue;
		const auto extension = cloth::lowercaseExtension(it->path());
		if (extension == ".co" || extension == ".clib") files.insert(it->path());
	}
}

int run (const std::vector<fs::path> &args) {
	InputDialect          dialect = InputDialect::Unknown;
	bool                  options = true;
	std::vector<fs::path> paths;
	for (const auto &arg : args) {
		const std::string spelling = cloth::pathUtf8(arg);
		if (options && spelling == "--help") {
			std::cout << USAGE;
			return 0;
		}
		if (options && spelling == "--")
			options = false;
		else if (options && spelling == "--from-legacy")
			dialect = InputDialect::Legacy;
		else if (options && spelling.starts_with('-')) {
			std::cerr << "Unknown option " << spelling << '\n' << USAGE;
			return 2;
		} else
			paths.push_back(arg);
	}
	if (paths.empty()) {
		std::cerr << USAGE;
		return 2;
	}
	std::set<fs::path> files;
	bool               success = true;
	for (const auto &path : paths) {
		try {
			collect(path, files);
		} catch (const std::exception &error) {
			std::cerr << cloth::pathUtf8(path) << ": " << error.what() << '\n';
			success = false;
		}
	}
	for (const auto &path : files) {
		try {
			const cloth::SourceFile source(path);
			const auto              replacement = cloth::migration::migrateSemicolons(source, dialect);
			if (replacement) cloth::publishFile(fs::canonical(path), std::as_bytes(std::span(*replacement)), &source);
			std::cout << (replacement ? "converted " : "unchanged ") << cloth::pathUtf8(path) << '\n';
		} catch (const cde::ClothDiagnostic &error) {
			cde::DiagnosticReporter{ std::cerr }.report(error);
			success = false;
		} catch (const std::exception &error) {
			std::cerr << cloth::pathUtf8(path) << ": " << error.what() << '\n';
			success = false;
		}
	}
	return success ? 0 : 1;
}
}

#ifdef _WIN32
int wmain (int argc, wchar_t *argv[]) {
#else
int main (int argc, char *argv[]) {
#endif
	return run({ argv + 1, argv + argc });
}
