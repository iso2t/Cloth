// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include <system/compiler_options.hpp>

namespace cloth::sys {

CompilerOptions::CompilerOptions(int argc, const char *const argv[]) {
	build_defaults();

	for (int index = 1; index < argc; ++index) {
		const std::string_view s_argument = argv[index];
		const auto bi_flag = std::ranges::find(v_compilerFlags, s_argument, &CompilerFlag::s_flag);
		if (bi_flag == v_compilerFlags.end()) {
			continue;
		}

		if (const auto *rr_value = std::get_if<CompilerFlag::Value>(&bi_flag->v_action)) {
			if (index + 1 == argc || argv[index + 1][0] == '\0') {
				throw std::invalid_argument(bi_flag->s_flag + " requires " + rr_value->s_description);
			}
			rr_value->f_func(argv[++index]);
		} else {
			std::get<std::function<void()> >(bi_flag->v_action)();
		}
	}
}

void CompilerOptions::add_flag(const CompilerFlag &flag) {
	v_compilerFlags.push_back(flag);
}

void CompilerOptions::build_defaults() {
	add_flag({ "-l", [this]() { i_lexer = 1; } });
	add_flag({ "-L", [this]() { i_lexer = 2; } });
	add_flag({ "--src", CompilerFlag::Value{ "a file path", [this](std::string_view path) { p_sourcePath.emplace(path); } } });
}

}
