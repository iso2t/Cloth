// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// disables the std::getenv deprecation warning.
// MSVC flags it as unsafe due to SDL (Security Development Lifecycle) best practices.
// It does risk thread safety due to raw pointers and undefined behaviors as it returns
// a non-const char*
#pragma warning(disable : 4996)

#include <iosfwd>

namespace cde {

class ClothDiagnostic;

class DiagnosticReporter {
    public:
	explicit DiagnosticReporter (std::ostream &output) : os_output(output) {
	}

	void report (const ClothDiagnostic &diagnostic) const;

    private:
	std::ostream &os_output;
};

}
