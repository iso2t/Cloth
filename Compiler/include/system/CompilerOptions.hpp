#include <functional>
#include <string>

namespace sys {

struct CompilerFlag {
	std::string flag;
	std::function<void()> func;
};

class CompilerOptions {};

}
