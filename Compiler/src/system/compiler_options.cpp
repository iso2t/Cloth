#include <system/compiler_options.hpp>

#include <algorithm>

namespace cloth::sys {

CompilerOptions::CompilerOptions(int argc, const char *const argv[]) {
	build_defaults();

	for (int index = 1; index < argc; ++index) {
		const auto flag = std::ranges::find(compiler_flags, argv[index], &CompilerFlag::flag);
		if (flag != compiler_flags.end()) {
			flag->func();
		}
	}
}

void CompilerOptions::add_flag(const CompilerFlag &flag) {
	compiler_flags.push_back(flag);
}

void CompilerOptions::build_defaults() {
	add_flag({ "-l", [this]() { LEXER = 1; } });
	add_flag({ "-L", [this]() { LEXER = 2; } });
}

}
