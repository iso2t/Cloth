// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <backend/backend.hpp>
#include <backend/lowered_module.hpp>

#include <cassert>
#include <climits>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <ast/decl/declarations.hpp>
#include <ast/expr/expressions.hpp>
#include <cde/cloth_diagnostic.hpp>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <semantic/checked_program.hpp>
#include <semantic/target_layout.hpp>
#include <token/numeric_literals.hpp>
#include <token/token.hpp>

namespace backend {
namespace {

using Code          = cde::DiagnosticCode;
using Message       = std::unique_ptr<char, decltype(&LLVMDisposeMessage)>;
using TargetMachine = std::unique_ptr<LLVMOpaqueTargetMachine, decltype(&LLVMDisposeTargetMachine)>;
using TargetData    = std::unique_ptr<LLVMOpaqueTargetData, decltype(&LLVMDisposeTargetData)>;
using MemoryBuffer  = std::unique_ptr<LLVMOpaqueMemoryBuffer, decltype(&LLVMDisposeMemoryBuffer)>;

LLVMValueRef lowerExpression (const cloth::Expression &expression, const cloth::ExpressionFacts &facts, LLVMTypeRef resultType) {
	if (!dynamic_cast<const cloth::IntegerLiteral *>(&expression)) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires an integer literal here",
		                           { { expression.get_span(), "unsupported expression" } });
	}
	const cloth::IntegerConstant &ic_value = std::get<cloth::IntegerConstant>(facts.at(&expression).numericConstant);
	// A literal checked against a signed 32-bit result fits in one magnitude word. Negation is a separate expression.
	assert(!ic_value.negative && ic_value.magnitude.size() == 1);
	return LLVMConstInt(resultType, ic_value.magnitude.front(), false);
}

void lowerEntryPoint (const cloth::CheckedProgram &program, LLVMModuleRef module) {
	if (!program.entryPoint) {
		throw cde::ClothDiagnostic(Code::INVALID_COMPILATION_REQUEST, "LLVM entry-point lowering requires a checked executable");
	}
	const cloth::Symbol          &sym_main = *program.entryPoint;
	const cloth::DeclarationInfo &di_main  = program.declarations.at(&sym_main);
	if (!di_main.parameters.empty() || di_main.returns != cloth::ResultTypes{ cloth::ResolvedType(cloth::kInteger) }) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_FUNCTION_ABI,
		                           std::format("LLVM lowering requires a parameterless entry point returning {}", cloth::to_string(cloth::kInteger)),
		                           { { sym_main.name.get_span(), "unsupported entry-point signature" } });
	}
	const cloth::FunctionBody   &fb_body = *std::get<const cloth::CallableSignature *>(sym_main.declaration)->executableBody();
	const cloth::ExpressionBody *eb_body = std::get_if<cloth::ExpressionBody>(&fb_body.value);
	if (!eb_body) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_LOWERING, "LLVM lowering requires an entry point with an expression body",
		                           { { fb_body.get_span(), "unsupported function body" } });
	}

	const LLVMContextRef ctx_context = LLVMGetModuleContext(module);
	const LLVMTypeRef    ty_int      = LLVMInt32TypeInContext(ctx_context);
	const LLVMValueRef   fn_main     = LLVMAddFunction(module, "main", LLVMFunctionType(ty_int, nullptr, 0, false));
	LLVMSetLinkage(fn_main, LLVMExternalLinkage);
	LLVMSetFunctionCallConv(fn_main, LLVMCCallConv);
	const std::unique_ptr<LLVMOpaqueBuilder, decltype(&LLVMDisposeBuilder)> ir_builder(LLVMCreateBuilderInContext(ctx_context), LLVMDisposeBuilder);
	LLVMPositionBuilderAtEnd(ir_builder.get(), LLVMAppendBasicBlockInContext(ctx_context, fn_main, "entry"));
	LLVMBuildRet(ir_builder.get(), lowerExpression(*eb_body->expression, program.expressions, ty_int));
}

void checkLLVMResult (LLVMBool failed, char *message, Code code, const char *operation) {
	const Message msg_detail(message, LLVMDisposeMessage);
	if (failed) {
		throw cde::ClothDiagnostic(code, msg_detail && *msg_detail ? std::format("{}\n{}", operation, msg_detail.get()) : std::string(operation));
	}
}

std::string toolchainTriple (unsigned sizeBits) {
	std::string s_triple(CLOTH_TOOLCHAIN_TRIPLE);
#if defined(__i386__) || defined(__x86_64__)
	// Only replace the architecture when switching widths. The vendor, OS, and ABI come from Cloth's toolchain.
	const unsigned u_archBits = s_triple.starts_with("x86_64") || s_triple.starts_with("amd64") ? 64u : 32u;
	if (sizeBits != u_archBits) s_triple.replace(0, s_triple.find('-'), sizeBits == 32 ? "i686" : "x86_64");
#else
	if (sizeBits != sizeof(void *) * CHAR_BIT) {
		throw cde::ClothDiagnostic(Code::UNSUPPORTED_TARGET, std::format("The {} toolchain has no configured {}-bit target", s_triple, sizeBits));
	}
#endif
	return s_triple;
}

TargetMachine createTargetMachine (std::string_view targetTriple, unsigned sizeBits) {
	static const bool b_initialized = LLVMInitializeNativeTarget() == 0 && LLVMInitializeNativeAsmPrinter() == 0;
	if (!b_initialized) throw cde::ClothDiagnostic(Code::LLVM_INITIALIZATION, "LLVM could not initialize the native target and assembly printer");

	if (targetTriple.find('\0') != std::string_view::npos) throw cde::ClothDiagnostic(Code::LLVM_TARGET, "A target triple contains a null byte");
	const std::string s_requested = targetTriple.empty() ? toolchainTriple(sizeBits) : std::string(targetTriple);
	const Message     s_triple(LLVMNormalizeTargetTriple(s_requested.c_str()), LLVMDisposeMessage);
	LLVMTargetRef     t_target = nullptr;
	char             *s_error  = nullptr;
	const LLVMBool    b_failed = LLVMGetTargetFromTriple(s_triple.get(), &t_target, &s_error);
	checkLLVMResult(b_failed, s_error, Code::LLVM_TARGET, "LLVM target lookup failed");

	TargetMachine tm_machine(LLVMCreateTargetMachine(t_target, s_triple.get(), "", "", LLVMCodeGenLevelNone, LLVMRelocDefault, LLVMCodeModelDefault),
	                         LLVMDisposeTargetMachine);
	if (!tm_machine) throw cde::ClothDiagnostic(Code::LLVM_TARGET, std::format("LLVM could not create a target machine for '{}'", s_triple.get()));
	return tm_machine;
}

void configureModule (LLVMModuleRef module, LLVMTargetMachineRef machine, const cloth::TargetLayout &checkedLayout) {
	const Message    s_triple(LLVMGetTargetMachineTriple(machine), LLVMDisposeMessage);
	const TargetData td_layout(LLVMCreateTargetDataLayout(machine), LLVMDisposeTargetData);
	const unsigned   u_pointerBits = LLVMPointerSize(td_layout.get()) * CHAR_BIT;
	if (u_pointerBits != checkedLayout.sizeBits) {
		throw cde::ClothDiagnostic(Code::ABI_LAYOUT, std::format("Target '{}' uses {}-bit pointers. The program was checked for {}-bit pointers",
		                                                         s_triple.get(), u_pointerBits, checkedLayout.sizeBits));
	}
	// Check the native integer alignment classes and the stride of the widest Cloth integer.
	for (const unsigned u_width : { 8u, 16u, 32u, 64u, 128u, cloth::MAX_INTEGER_BITS }) {
		const LLVMTypeRef          ty_int = LLVMIntTypeInContext(LLVMGetModuleContext(module), u_width);
		const cloth::StorageLayout sl_actual{ LLVMABISizeOfType(td_layout.get(), ty_int), LLVMABIAlignmentOfType(td_layout.get(), ty_int) };
		const cloth::StorageLayout sl_expected = checkedLayout.integerStorage(u_width);
		if (sl_actual != sl_expected) {
			throw cde::ClothDiagnostic(
			        Code::ABI_LAYOUT,
			        std::format("Target '{}' gives i{} size {} and alignment {}. The checked layout requires size {} and alignment {}",
			                    s_triple.get(), u_width, sl_actual.size, sl_actual.alignment, sl_expected.size, sl_expected.alignment));
		}
	}
	LLVMSetTarget(module, s_triple.get());
	LLVMSetModuleDataLayout(module, td_layout.get());
}

}

LoweredModule::LoweredModule (const cloth::CheckedProgram &program) :
        ctx_context(LLVMContextCreate(), LLVMContextDispose),
        mod_module(LLVMModuleCreateWithNameInContext("cloth", ctx_context.get()), LLVMDisposeModule) {
	lowerEntryPoint(program, mod_module.get());
}

LLVMModuleRef LoweredModule::get () const {
	return mod_module.get();
}

void LoweredModule::verify () const {
	char          *s_error  = nullptr;
	const LLVMBool b_failed = LLVMVerifyModule(get(), LLVMReturnStatusAction, &s_error);
	checkLLVMResult(b_failed, s_error, Code::INVALID_IR, "LLVM module verification failed");
}

std::vector<std::byte> emitObject (const cloth::CheckedProgram &program, std::string_view targetTriple) {
	const LoweredModule lm_module(program);
	const TargetMachine tm_machine = createTargetMachine(targetTriple, program.layout.sizeBits);
	configureModule(lm_module.get(), tm_machine.get(), program.layout);
	lm_module.verify();

	char               *s_error   = nullptr;
	LLVMMemoryBufferRef mb_output = nullptr;
	const LLVMBool      b_failed  = LLVMTargetMachineEmitToMemoryBuffer(tm_machine.get(), lm_module.get(), LLVMObjectFile, &s_error, &mb_output);
	const MemoryBuffer  mb_object(mb_output, LLVMDisposeMemoryBuffer);
	checkLLVMResult(b_failed, s_error, Code::LLVM_OBJECT, "LLVM object emission failed");
	if (!mb_object || LLVMGetBufferSize(mb_object.get()) == 0) {
		throw cde::ClothDiagnostic(Code::LLVM_OBJECT, "LLVM returned an empty object");
	}
	const std::byte *p_bytes = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(mb_object.get()));
	return { p_bytes, p_bytes + LLVMGetBufferSize(mb_object.get()) };
}

}
