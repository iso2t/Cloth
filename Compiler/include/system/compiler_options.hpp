#pragma once

#include <functional>
#include <string>
#include <vector>

namespace cloth::sys {

struct CompilerFlag {
	std::string flag;
	std::function<void()> func;
};

class CompilerOptions {
    public:
	CompilerOptions(int argc, const char *const argv[]);

	~CompilerOptions() = default;

	std::vector<CompilerFlag> get_flags() const {
		return compiler_flags;
	}

	void add_flag(const CompilerFlag &flag);
	void build_defaults();

	int LEXER = 0;

    private:
	std::vector<CompilerFlag> compiler_flags;
};

}
