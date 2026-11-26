// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace cloth {

struct NativeModules {
	std::string                                  name;
	std::map<std::string, std::filesystem::path> imports;
	bool                                         recordExternInterfaces = false;
};

}
