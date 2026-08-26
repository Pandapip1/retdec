/**
* @file tests/llvmir2hll/llvm/llvmir2bir_converters/orig_llvmir2bir_converter_tests.cpp
* @brief Tests for the @c orig_llvmir2bir_converter module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/llvm/llvmir2bir_converters/orig_llvmir2bir_converter.h"
#include "llvmir2hll/llvm/llvmir2bir_converter_tests.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"
#include "retdec/llvmir2hll/utils/ir.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class RecursionFallbackFinder final: private OrderedAllVisitor {
public:
	static bool hasFallback(ShPtr<Statement> statement) {
		RecursionFallbackFinder finder;
		finder.visitStmt(statement);
		return finder.found;
	}

private:
	RecursionFallbackFinder(): OrderedAllVisitor(), found(false) {}

	using OrderedAllVisitor::visit;
	void visit(ShPtr<GotoStmt> statement) override {
		if (statement->getMetadata().find("goto recursion fallback -> ") == 0) {
			found = true;
		}
		OrderedAllVisitor::visit(statement);
	}

private:
	bool found;
};

/**
* @brief Tests for the @c orig_llvmir2bir_converter module.
*/
class OrigLLVMIR2BIRConverterTests: public LLVMIR2BIRConverterTests {
protected:
	ShPtr<Module> convertLLVMIR2BIR(const std::string &code);
};

ShPtr<Module> OrigLLVMIR2BIRConverterTests::convertLLVMIR2BIR(
		const std::string &code) {
	return LLVMIR2BIRConverterTests::convertLLVMIR2BIR<OrigLLVMIR2BIRConverter>(code);
}

//
// Global variables.
//

TEST_F(OrigLLVMIR2BIRConverterTests,
IntegralGlobalVariableWithInitializerIsConvertedCorrectly) {
	auto module = convertLLVMIR2BIR(R"(
		@g = global i32 0
	)");

	auto g = module->getGlobalVarByName("g");
	ASSERT_TRUE(g);
	auto gType = cast<IntType>(g->getType());
	ASSERT_TRUE(gType);
	ASSERT_EQ(32, gType->getSize());
	auto gInit = cast<ConstInt>(module->getInitForGlobalVar(g));
	ASSERT_TRUE(gInit);
	ASSERT_EQ(0, gInit->getValue());
}

//
// Control flow.
//

TEST_F(OrigLLVMIR2BIRConverterTests,
SwitchDefaultBackEdgeToCurrentLoopIsConvertedToContinue) {
	auto module = convertLLVMIR2BIR(R"(
		define i32 @function(i32 %selector, i32 %innerSelector) {
		entry:
			br label %loop
		loop:
			switch i32 %selector, label %inner [
				i32 0, label %exit
			]
		inner:
			switch i32 %innerSelector, label %loop [
				i32 0, label %exit
			]
		exit:
			ret i32 0
		}
	)");

	auto function = module->getFuncByName("function");
	ASSERT_TRUE(function);
	auto loop = cast<WhileLoopStmt>(skipEmptyStmts(function->getBody()));
	ASSERT_TRUE(loop);
	auto outerSwitch = cast<SwitchStmt>(skipEmptyStmts(loop->getBody()));
	ASSERT_TRUE(outerSwitch);
	auto innerSwitch = cast<SwitchStmt>(
		skipEmptyStmts(outerSwitch->getDefaultClauseBody()));
	ASSERT_TRUE(innerSwitch);
	auto continueStmt = cast<ContinueStmt>(
		skipEmptyStmts(innerSwitch->getDefaultClauseBody()));
	ASSERT_TRUE(continueStmt);
	ASSERT_EQ("continue -> loop", continueStmt->getMetadata());
}

TEST_F(OrigLLVMIR2BIRConverterTests,
RepeatedSwitchTargetRecursionIsPreservedAsGoto) {
	auto module = convertLLVMIR2BIR(R"(
		define i32 @function(i32 %selector) {
		entry:
			switch i32 %selector, label %exit [
				i32 0, label %shared
				i32 1, label %shared
				i32 2, label %shared
				i32 3, label %shared
				i32 4, label %shared
				i32 5, label %shared
				i32 6, label %shared
				i32 7, label %shared
				i32 8, label %shared
				i32 9, label %shared
				i32 10, label %shared
				i32 11, label %shared
				i32 12, label %shared
				i32 13, label %shared
				i32 14, label %shared
				i32 15, label %shared
				i32 16, label %shared
				i32 17, label %shared
				i32 18, label %shared
				i32 19, label %shared
				i32 20, label %shared
				i32 21, label %shared
				i32 22, label %shared
				i32 23, label %shared
				i32 24, label %shared
				i32 25, label %shared
			]
		shared:
			ret i32 1
		exit:
			ret i32 0
		}
	)");

	auto function = module->getFuncByName("function");
	ASSERT_TRUE(function);
	ASSERT_TRUE(RecursionFallbackFinder::hasFallback(function->getBody()));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
