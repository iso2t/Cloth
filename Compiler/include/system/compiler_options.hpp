#include <functional>
#include <string>

namespace cloth::sys {

struct CompilerFlag {
	std::string flag;
	std::function<void()> func;
};

class CompilerOptions {};

}
