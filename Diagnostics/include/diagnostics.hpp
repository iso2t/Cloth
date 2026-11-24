// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Due to laziness, this file includes all diagnostic-related headers.
// This should be updated and used in place of importing each header individually.
// Due to pragma once, this will define everything early on and we should be ok.

#pragma once

#include <cde/cloth_diagnostic.hpp>
#include <cde/declaration_errors.hpp>
#include <cde/diagnostic_code.hpp>
#include <cde/diagnostic_reporter.hpp>
#include <cde/generic_errors.hpp>
#include <cde/import_errors.hpp>
#include <cde/syntax_errors.hpp>
#include <cde/syntax_exception.hpp>
