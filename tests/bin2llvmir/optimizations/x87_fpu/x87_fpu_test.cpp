/**
* @file tests/bin2llvmir/optimizations/x87_fpu/x87_fpu.cpp
* @brief Tests for the @c X87FpuAnalysis.
* @copyright (c) 2019 Avast Software, licensed under the MIT license
*/

#include <set>

#include <llvm/IR/InstIterator.h>

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_pass.h"
#include "retdec/bin2llvmir/optimizations/x87_fpu/x87_fpu.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/utils/string.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {


/**
 * @brief Tests for the @c X87FpuAnalysis.
 */
class X87FpuAnalysisTests: public LlvmIrTests
{
protected:
	Abi *abi;
	retdec::config::Config c;
	Config *config;

	X87FpuAnalysis pass;
	void setX86Environment(std::string architecture, std::string callingConvention);

	const std::string PREDEFINED_REGISTERS_AND_FUNCTIONS = R"(
		@fpu_stat_TOP = internal global i3 0
		@st0 = internal global x86_fp80 0xK00000000000000000000
		@st1 = internal global x86_fp80 0xK00000000000000000000
		@st2 = internal global x86_fp80 0xK00000000000000000000
		@st3 = internal global x86_fp80 0xK00000000000000000000
		@st4 = internal global x86_fp80 0xK00000000000000000000
		@st5 = internal global x86_fp80 0xK00000000000000000000
		@st6 = internal global x86_fp80 0xK00000000000000000000
		@st7 = internal global x86_fp80 0xK00000000000000000000

		declare void @__frontend_reg_store.fpr(i3, x86_fp80)
		declare x86_fp80 @__frontend_reg_load.fpr(i3)
	)";
}; //X87FpuAnalysisTests

void X87FpuAnalysisTests::setX86Environment(std::string architecture, std::string callingConvention)
{
	c = config::Config::fromJsonString(R"({
		"architecture" : {
			"bitSize" : )" + architecture + R"(,
			"endian" : "little",
			"name" : "x86"
		},
		"mainAddress" : "0x1000",
		"functions" :
		[
			{
				"callingConvention" : ")" + callingConvention + R"(",
				"name" : "foo"
			},
			{
				"callingConvention" : ")" + callingConvention + R"(",
				"name" : "boo"
			}
		]
	})");
	config = ConfigProvider::addConfig(module.get(), c);

	config->setLlvmX87DataStorePseudoFunction(getFunctionByName("__frontend_reg_store.fpr"));
	config->setLlvmX87DataLoadPseudoFunction(getFunctionByName("__frontend_reg_load.fpr"));

	abi = AbiProvider::addAbi(module.get(), config);
	abi->addRegister(X87_REG_TOP, getGlobalByName("fpu_stat_TOP"));

	unsigned numberOfFpuRegisters = 8;
	for (unsigned i = 0; i < numberOfFpuRegisters; i++)
	{
		abi->addRegister(X86_REG_ST0 + i, getGlobalByName("st"+std::to_string(i)));
	}
}

// Architecture: 		16bit
// Calling convention: 	cdecl
// Operation:			Call function with floating-point return value.

TEST_F(X87FpuAnalysisTests, x86_16bit_cdecl_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = add i3 %2, 1
			store i3 %3, i3* @fpu_stat_TOP
			ret void
		}
		define void @boo() {
		bb:
			; ...
			call void @foo()
			; fp return value is saved in memory and addr is in AX -> 16bit cdecl convention
			; ...
			ret void
		})");

	setX86Environment("16", "cdecl");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = add i3 %2, 1
		  store i3 %3, i3* @fpu_stat_TOP
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_16bit_cdecl_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_16bit_cdecl_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			; ...
			call void @foo()
			; ...
			ret void
		}
		define void @foo() {
		bb:
			; ...
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = call x86_fp80 @__frontend_reg_load.fpr(i3 %2)
			%4 = add i3 %2, 1
			store i3 %4, i3* @fpu_stat_TOP
			; ...
			ret void
		})");

		setX86Environment("16", "cdecl");
		bool b = pass.runOnModuleCustom(*module, config, abi);

		std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		}
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = load x86_fp80, x86_fp80* @st7
		  %4 = add i3 %2, 1
		  store i3 %4, i3* @fpu_stat_TOP
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_16bit_cdecl_call_of_not_analyzed_function_success

// Architecture: 		16bit
// Calling convention: 	pascal
// Operation:			Call function with floating-point return value.

TEST_F(X87FpuAnalysisTests, x86_16bit_pascal_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			call void @__frontend_reg_store.fpr(i3 %1, x86_fp80 0xK3FFF8000000000000000)
			%2 = sub i3 %0, 2
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			store i3 %2, i3* @fpu_stat_TOP
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3)
			%5 = add i3 %3, 1
			%6 = add i3 %3, 2
			store i3 %6, i3* @fpu_stat_TOP
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			ret void
		})");

	setX86Environment("16", "pascal");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %2 = sub i3 %0, 2
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  store i3 %2, i3* @fpu_stat_TOP
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st6
		  %5 = add i3 %3, 1
		  %6 = add i3 %3, 2
		  store i3 %6, i3* @fpu_stat_TOP
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_16bit_pascal_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_16bit_pascal_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			call void @foo()
			ret void
		}
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			%2 = sub i3 %1, 1
			call void @__frontend_reg_store.fpr(i3 %1, x86_fp80 0xK3FFF8000000000000000)
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			store i3 %2, i3* @fpu_stat_TOP
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3)
			%5 = add i3 %3, 1
			%6 = add i3 %5, 1
			store i3 %6, i3* @fpu_stat_TOP
			ret void
		})");

	setX86Environment("16", "pascal");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		}
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  %2 = sub i3 %1, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  store i3 %2, i3* @fpu_stat_TOP
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st6
		  %5 = add i3 %3, 1
		  %6 = add i3 %5, 1
		  store i3 %6, i3* @fpu_stat_TOP
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_16bit_pascal_call_of_not_analyzed_function_success

//
// Architecture: 		16bit
// Calling convention: 	fastcall
// Operation:			Call function returning floating-point value.
//

TEST_F(X87FpuAnalysisTests, x86_16bit_fastcall_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = sub i3 %2, 1
			store i3 %3, i3* @fpu_stat_TOP
			%4 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %4, x86_fp80 0xK3FFF8000000000000000)
			%5 = call x86_fp80 @__frontend_reg_load.fpr(i3 %4)
			%6 = add i3 %4, 1
			store i3 %6, i3* @fpu_stat_TOP
			%7 = load i3, i3* @fpu_stat_TOP
			%8 = call x86_fp80 @__frontend_reg_load.fpr(i3 %7)
			%9 = add i3 %7, 1
			store i3 %9, i3* @fpu_stat_TOP
			%10 = load i3, i3* @fpu_stat_TOP
			%11 = call x86_fp80 @__frontend_reg_load.fpr(i3 %10)
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			ret void
		})");

	setX86Environment("16", "fastcall");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = sub i3 %2, 1
		  store i3 %3, i3* @fpu_stat_TOP
		  %4 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %5 = load x86_fp80, x86_fp80* @st6
		  %6 = add i3 %4, 1
		  store i3 %6, i3* @fpu_stat_TOP
		  %7 = load i3, i3* @fpu_stat_TOP
		  %8 = load x86_fp80, x86_fp80* @st7
		  %9 = add i3 %7, 1
		  store i3 %9, i3* @fpu_stat_TOP
		  %10 = load i3, i3* @fpu_stat_TOP
		  %11 = load x86_fp80, x86_fp80* @st0
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_16bit_fastcall_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_16bit_fastcall_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			call void @foo()
			ret void
		}
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 3
			%2 = sub i3 %1, 3
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = sub i3 %2, 2
			call void @__frontend_reg_store.fpr(i3 %3, x86_fp80 0xK3FFF8000000000000000)
			%4 = sub i3 %3, 2
			call void @__frontend_reg_store.fpr(i3 %4, x86_fp80 0xK3FFF8000000000000000)
			store i3 %4, i3* @fpu_stat_TOP
			%5 = load i3, i3* @fpu_stat_TOP
			%6 = call x86_fp80 @__frontend_reg_load.fpr(i3 %5)
			%7 = add i3 %5, 3
			%8 = add i3 %7, 3; can not add 10 cause of 3bit overflow
			%9 = call x86_fp80 @__frontend_reg_load.fpr(i3 %8)
			%10 = add i3 %8, 2
			%11 = add i3 %10, 2
			%12 = call x86_fp80 @__frontend_reg_load.fpr(i3 %11)
			store i3 %11, i3* @fpu_stat_TOP
			ret void
		})");

	setX86Environment("16", "fastcall");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		}
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 3
		  %2 = sub i3 %1, 3
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st2
		  %3 = sub i3 %2, 2
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %4 = sub i3 %3, 2
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  store i3 %4, i3* @fpu_stat_TOP
		  %5 = load i3, i3* @fpu_stat_TOP
		  %6 = load x86_fp80, x86_fp80* @st6
		  %7 = add i3 %5, 3
		  %8 = add i3 %7, 3
		  %9 = load x86_fp80, x86_fp80* @st4
		  %10 = add i3 %8, 2
		  %11 = add i3 %10, 2
		  %12 = load x86_fp80, x86_fp80* @st0
		  store i3 %11, i3* @fpu_stat_TOP
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_16bit_fastcall_call_of_not_analyzed_function_success

//
// Architecture: 		32bit
// Calling convention: 	cdecl
// Operation:			Call function with floating-point return value.
//

TEST_F(X87FpuAnalysisTests, x86_32bit_cdecl_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = call x86_fp80 @__frontend_reg_load.fpr(i3 %0)
			%2 = add i3 %0, 1
			store i3 %2, i3* @fpu_stat_TOP
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3)
			ret void
		})");

	setX86Environment("32", "cdecl");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  %1 = load x86_fp80, x86_fp80* @st7
		  %2 = add i3 %0, 1
		  store i3 %2, i3* @fpu_stat_TOP
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st0
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_cdecl_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_32bit_cdecl_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = call x86_fp80 @__frontend_reg_load.fpr(i3 %0)
			%2 = add i3 %0, 1
			store i3 %2, i3* @fpu_stat_TOP
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3)
			ret void
		}
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			ret void
		})");

	setX86Environment("32", "cdecl");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  %1 = load x86_fp80, x86_fp80* @st7
		  %2 = add i3 %0, 1
		  store i3 %2, i3* @fpu_stat_TOP
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st0
		  ret void
		}
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_cdecl_call_of_not_analyzed_function_success


//
// Architecture: 		32bit
// Calling convention: 	stdcall
// Operation:			Call function with floating-point arguments and return value.
//

TEST_F(X87FpuAnalysisTests, x86_32bit_stdcall_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = sub i3 %2, 1
			store i3 %3, i3* @fpu_stat_TOP
			%4 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %4, x86_fp80 0xK3FFF8000000000000000)
			%5 = call x86_fp80 @__frontend_reg_load.fpr(i3 %4)
			%6 = add i3 %4, 1
			store i3 %6, i3* @fpu_stat_TOP
			%7 = load i3, i3* @fpu_stat_TOP
			%8 = call x86_fp80 @__frontend_reg_load.fpr(i3 %7)
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = call x86_fp80 @__frontend_reg_load.fpr(i3 %0)
			%2 = add i3 %0, 1
			store i3 %2, i3* @fpu_stat_TOP
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3)
			ret void
		})");

	setX86Environment("32", "stdcall");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = sub i3 %2, 1
		  store i3 %3, i3* @fpu_stat_TOP
		  %4 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %5 = load x86_fp80, x86_fp80* @st6
		  %6 = add i3 %4, 1
		  store i3 %6, i3* @fpu_stat_TOP
		  %7 = load i3, i3* @fpu_stat_TOP
		  %8 = load x86_fp80, x86_fp80* @st7
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  %1 = load x86_fp80, x86_fp80* @st7
		  %2 = add i3 %0, 1
		  store i3 %2, i3* @fpu_stat_TOP
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st0
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_stdcall_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_32bit_stdcall_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = call x86_fp80 @__frontend_reg_load.fpr(i3 %0)
			%2 = add i3 %0, 1
			store i3 %2, i3* @fpu_stat_TOP
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3)
			ret void
		}
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			call void @__frontend_reg_store.fpr(i3 %1, x86_fp80 0xK3FFF8000000000000000)
			%2 = sub i3 %0, 2
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			store i3 %1, i3* @fpu_stat_TOP
			ret void
		})");

	setX86Environment("32", "stdcall");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  %1 = load x86_fp80, x86_fp80* @st7
		  %2 = add i3 %0, 1
		  store i3 %2, i3* @fpu_stat_TOP
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st0
		  ret void
		}
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %2 = sub i3 %0, 2
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  store i3 %1, i3* @fpu_stat_TOP
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_stdcall_call_of_not_analyzed_function_success

//
// Architecture: 		32bit
// Calling convention: 	pascal
// Operation:			Call function with floating-point arguments and return value.
//

TEST_F(X87FpuAnalysisTests, x86_32bit_pascal_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			call void @__frontend_reg_store.fpr(i3 %1, x86_fp80 0xK3FFF8000000000000000)
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			call void @__frontend_reg_store.fpr(i3 %1, x86_fp80 0xK3FFF8000000000000000)
			ret void
		})");

	setX86Environment("32", "pascal");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_pascal_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_32bit_pascal_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = call x86_fp80 @__frontend_reg_load.fpr(i3 %0)
			%2 = add i3 %0, 1
			%3 = call x86_fp80 @__frontend_reg_load.fpr(i3 %2)
			store i3 %2, i3* @fpu_stat_TOP
			ret void
		}
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			br i1 1, label %A, label %B
		A:
			%2 = sub i3 %1, 1
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			br label %C
		B:
			%3 = sub i3 %1, 2
			call void @__frontend_reg_store.fpr(i3 %3, x86_fp80 0xK3FFF8000000000000000)
			br label %C
		C:
			store i3 %1, i3* @fpu_stat_TOP
			ret void
		})");

	setX86Environment("32", "pascal");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::set<Value*> stackRegisters;
	for (unsigned n = 0; n < 8; ++n)
	{
		stackRegisters.insert(getGlobalByName("st" + std::to_string(n)));
	}

	auto* foo = getFunctionByName("foo");
	unsigned pseudoCalls = 0;
	for (auto& instruction : instructions(foo))
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataStorePseudoFunction();
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataLoadPseudoFunction();
		}
	}
	EXPECT_EQ(0u, pseudoCalls);

	for (auto* blockName : {"A", "B"})
	{
		BasicBlock* block = nullptr;
		for (auto& candidate : *foo)
		{
			if (candidate.getName() == blockName)
			{
				block = &candidate;
				break;
			}
		}
		ASSERT_NE(nullptr, block);
		unsigned runtimeSelects = 0;
		unsigned stackStores = 0;
		for (auto& instruction : *block)
		{
			runtimeSelects += isa<SelectInst>(&instruction);
			if (auto* store = dyn_cast<StoreInst>(&instruction))
			{
				stackStores += stackRegisters.count(store->getPointerOperand());
			}
		}
		EXPECT_EQ(8u, runtimeSelects);
		EXPECT_EQ(8u, stackStores);
	}
	EXPECT_FALSE(verifyModule(*module, &errs()));
	EXPECT_TRUE(b);
} // x86_32bit_pascal_call_of_not_analyzed_function_success

//
// Architecture: 		32bit
// Calling convention: 	fastcall
// Operation:			Call function with floating-point arguments and return value.
//

TEST_F(X87FpuAnalysisTests, x86_32bit_fastcall_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 2
			store i3 %1, i3* @fpu_stat_TOP
			br i1 1, label %A, label %B
		A:
			%2 = load i3, i3* @fpu_stat_TOP
			%3 = add i3 %2, 1
			call void @__frontend_reg_store.fpr(i3 %3, x86_fp80 0xK3FFF8000000000000000)
			store i3 %3, i3* @fpu_stat_TOP
			br label %C
		B:
			%4 = load i3, i3* @fpu_stat_TOP
			%5 = add i3 %4, 1
			call void @__frontend_reg_store.fpr(i3 %5, x86_fp80 0xK3FFF8000000000000000)
			store i3 %5, i3* @fpu_stat_TOP
			br label %C
		C:
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = call x86_fp80 @__frontend_reg_load.fpr(i3 %0)
			%2 = add i3 %0, 1
			%3 = call x86_fp80 @__frontend_reg_load.fpr(i3 %2)
			store i3 %2, i3* @fpu_stat_TOP
			ret void
		})");

	setX86Environment("32", "fastcall");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 2
		  store i3 %1, i3* @fpu_stat_TOP
		  br i1 true, label %A, label %B
		A:
		  %2 = load i3, i3* @fpu_stat_TOP
		  %3 = add i3 %2, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  store i3 %3, i3* @fpu_stat_TOP
		  br label %C
		B:
		  %4 = load i3, i3* @fpu_stat_TOP
		  %5 = add i3 %4, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  store i3 %5, i3* @fpu_stat_TOP
		  br label %C
		C:
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  %1 = load x86_fp80, x86_fp80* @st7
		  %2 = add i3 %0, 1
		  %3 = load x86_fp80, x86_fp80* @st0
	      store i3 %2, i3* @fpu_stat_TOP
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_fastcall_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_32bit_fastcall_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			ret void
		}
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 2
			store i3 %1, i3* @fpu_stat_TOP
			br i1 1, label %A, label %B
		A:
			%2 = load i3, i3* @fpu_stat_TOP
			%3 = add i3 %2, 1
			call void @__frontend_reg_store.fpr(i3 %3, x86_fp80 0xK3FFF8000000000000000)
			store i3 %3, i3* @fpu_stat_TOP
			br label %C
		B:
			%4 = load i3, i3* @fpu_stat_TOP
			%5 = add i3 %4, 1
			call void @__frontend_reg_store.fpr(i3 %5, x86_fp80 0xK3FFF8000000000000000)
			store i3 %5, i3* @fpu_stat_TOP
			br label %C
		C:
			%6 = load i3, i3* @fpu_stat_TOP
			%7 = add i3 %6, 1
			call void @__frontend_reg_store.fpr(i3 %7, x86_fp80 0xK3FFF8000000000000000)
			store i3 %7, i3* @fpu_stat_TOP
			ret void
		})");

	setX86Environment("32", "fastcall");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  ret void
		}
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 2
		  store i3 %1, i3* @fpu_stat_TOP
		  br i1 true, label %A, label %B
		A:
		  %2 = load i3, i3* @fpu_stat_TOP
		  %3 = add i3 %2, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  store i3 %3, i3* @fpu_stat_TOP
		  br label %C
		B:
		  %4 = load i3, i3* @fpu_stat_TOP
		  %5 = add i3 %4, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  store i3 %5, i3* @fpu_stat_TOP
		  br label %C
		C:
		  %6 = load i3, i3* @fpu_stat_TOP
		  %7 = add i3 %6, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  store i3 %7, i3* @fpu_stat_TOP
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_fastcall_call_of_not_analyzed_function_success

//
// Architecture: 		32bit
// Calling convention: 	thiscall
// Operation:			Call function with floating-point arguments and return value.
//

TEST_F(X87FpuAnalysisTests, x86_32bit_thiscall)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			call void @__frontend_reg_store.fpr(i3 %1, x86_fp80 0xK3FFF8000000000000000)
			store i3 %1, i3* @fpu_stat_TOP
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = call x86_fp80 @__frontend_reg_load.fpr(i3 %0)
			%2 = add i3 %0, 1
			%3 = call x86_fp80 @__frontend_reg_load.fpr(i3 %2)
			store i3 %2, i3* @fpu_stat_TOP
			ret void
		})");

	setX86Environment("32", "thiscall");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  store i3 %1, i3* @fpu_stat_TOP
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  %1 = load x86_fp80, x86_fp80* @st7
		  %2 = add i3 %0, 1
		  %3 = load x86_fp80, x86_fp80* @st0
		  store i3 %2, i3* @fpu_stat_TOP
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_thiscall

//
// Architecture: 		32bit
// Calling convention: 	watcom
// Operation:			Call function with floating-point arguments and return value.
//

TEST_F(X87FpuAnalysisTests, x86_32bit_watcom)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 3
			call void @__frontend_reg_store.fpr(i3 %1, x86_fp80 0xK3FFF8000000000000000)
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = add i3 %0, 2
			%2 = call x86_fp80 @__frontend_reg_load.fpr(i3 %1)
			ret void
		})");

	setX86Environment("32", "watcom");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 3
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st5
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = add i3 %0, 2
		  %2 = load x86_fp80, x86_fp80* @st2
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_watcom

//
// Architecture: 		32bit
// Calling convention: 	unknown
// Operation:			Call function without floating-point arguments and return value.
//

TEST_F(X87FpuAnalysisTests, x86_32bit_analyze_not_FP_return_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = call x86_fp80 @__frontend_reg_load.fpr(i3 %2)
			%4 = add i3 %2, 1
			store i3 %4, i3* @fpu_stat_TOP
			%5 = load i3, i3* @fpu_stat_TOP
			%6 = call x86_fp80 @__frontend_reg_load.fpr(i3 %5)
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			ret void
		})");

	setX86Environment("32", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = load x86_fp80, x86_fp80* @st7
		  %4 = add i3 %2, 1
		  store i3 %4, i3* @fpu_stat_TOP
		  %5 = load i3, i3* @fpu_stat_TOP
		  %6 = load x86_fp80, x86_fp80* @st0
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_32bit_analyze_not_FP_return_success

//
// Architecture: 		64bit
// Calling convention: 	x64 windows, linux, bsd, mac
// Operation:			Call function with floating-point arguments and return value.
//

TEST_F(X87FpuAnalysisTests, x86_64bit_call_of_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = call x86_fp80 @__frontend_reg_load.fpr(i3 %2)
			%4 = add i3 %2, 1
			store i3 %4, i3* @fpu_stat_TOP
			%5 = load i3, i3* @fpu_stat_TOP
			%6 = call x86_fp80 @__frontend_reg_load.fpr(i3 %5); this val will be saved to xmm0
			ret void
		}
		define void @boo() {
		bb:
			call void @foo()
			ret void
		})");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = load x86_fp80, x86_fp80* @st7
		  %4 = add i3 %2, 1
		  store i3 %4, i3* @fpu_stat_TOP
		  %5 = load i3, i3* @fpu_stat_TOP
		  %6 = load x86_fp80, x86_fp80* @st0
		  ret void
		}
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_64bit_call_of_analyzed_function_success

TEST_F(X87FpuAnalysisTests, x86_64bit_call_of_not_analyzed_function_success)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
			call void @foo()
			ret void
		}
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = call x86_fp80 @__frontend_reg_load.fpr(i3 %2);
			%4 = add i3 %2, 1
			store i3 %4, i3* @fpu_stat_TOP
			%5 = load i3, i3* @fpu_stat_TOP
			%6 = call x86_fp80 @__frontend_reg_load.fpr(i3 %5); this val will be saved to xmm0
			ret void
		})");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @boo() {
		bb:
		  call void @foo()
		  ret void
		}
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = load x86_fp80, x86_fp80* @st7
		  %4 = add i3 %2, 1
		  store i3 %4, i3* @fpu_stat_TOP
		  %5 = load i3, i3* @fpu_stat_TOP
		  %6 = load x86_fp80, x86_fp80* @st0
		  ret void
		})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // x86_64bit_call_of_not_analyzed_function_success

//
// BRANCH AND LOOPS
//

TEST_F(X87FpuAnalysisTests, if_branch_or_loop)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			br i1 1, label %dec_label_if_true, label %dec_label_end_branch
		dec_label_if_true:
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3);
			%5 = sub i3 %3, 1
			store i3 %5, i3* @fpu_stat_TOP
			%6 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %6, x86_fp80 0xK3FFF8000000000000000)
			%7 = call x86_fp80 @__frontend_reg_load.fpr(i3 %6);
			%8 = add i3 %6, 1
			store i3 %8, i3* @fpu_stat_TOP
			br label %dec_label_end_branch
		dec_label_end_branch:
			%9 = load i3, i3* @fpu_stat_TOP
			%10 = call x86_fp80 @__frontend_reg_load.fpr(i3 %9);
			%11 = add i3 %9, 1
			store i3 %11, i3* @fpu_stat_TOP
			%12 = load i3, i3* @fpu_stat_TOP
			%13 = call x86_fp80 @__frontend_reg_load.fpr(i3 %12);
			ret void
		})");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  br i1 true, label %dec_label_if_true, label %dec_label_end_branch
		dec_label_if_true:
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st7
		  %5 = sub i3 %3, 1
		  store i3 %5, i3* @fpu_stat_TOP
		  %6 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %7 = load x86_fp80, x86_fp80* @st6
		  %8 = add i3 %6, 1
		  store i3 %8, i3* @fpu_stat_TOP
		  br label %dec_label_end_branch
		dec_label_end_branch:
		  %9 = load i3, i3* @fpu_stat_TOP
		  %10 = load x86_fp80, x86_fp80* @st7
		  %11 = add i3 %9, 1
		  store i3 %11, i3* @fpu_stat_TOP
		  %12 = load i3, i3* @fpu_stat_TOP
		  %13 = load x86_fp80, x86_fp80* @st0
		  ret void
})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // if_branch

TEST_F(X87FpuAnalysisTests, if_else_branch)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			br i1 1, label %dec_label_if_true, label %dec_label_if_false
		dec_label_if_true:
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3);
			%5 = sub i3 %3, 1
			store i3 %5, i3* @fpu_stat_TOP
			%6 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %6, x86_fp80 0xK3FFF8000000000000000)
			%7 = call x86_fp80 @__frontend_reg_load.fpr(i3 %6);
			%8 = add i3 %6, 1
			store i3 %8, i3* @fpu_stat_TOP
			br label %dec_label_end_branch
		dec_label_if_false:
			%9 = load i3, i3* @fpu_stat_TOP
			%10 = call x86_fp80 @__frontend_reg_load.fpr(i3 %9);
			%11 = sub i3 %9, 1
			store i3 %11, i3* @fpu_stat_TOP
			%12 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %12, x86_fp80 0xK3FFF8000000000000000)
			%13 = call x86_fp80 @__frontend_reg_load.fpr(i3 %12);
			%14 = add i3 %12, 1
			store i3 %14, i3* @fpu_stat_TOP
			br label %dec_label_end_branch
		dec_label_end_branch:
			%15 = load i3, i3* @fpu_stat_TOP
			%16 = call x86_fp80 @__frontend_reg_load.fpr(i3 %15);
			%17 = add i3 %15, 1
			store i3 %17, i3* @fpu_stat_TOP
			%18 = load i3, i3* @fpu_stat_TOP
			%19 = call x86_fp80 @__frontend_reg_load.fpr(i3 %18);
			ret void
		}
)");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  br i1 true, label %dec_label_if_true, label %dec_label_if_false
		dec_label_if_true:
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st7
		  %5 = sub i3 %3, 1
		  store i3 %5, i3* @fpu_stat_TOP
		  %6 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %7 = load x86_fp80, x86_fp80* @st6
		  %8 = add i3 %6, 1
		  store i3 %8, i3* @fpu_stat_TOP
		  br label %dec_label_end_branch
		dec_label_if_false:
		  %9 = load i3, i3* @fpu_stat_TOP
		  %10 = load x86_fp80, x86_fp80* @st7
		  %11 = sub i3 %9, 1
		  store i3 %11, i3* @fpu_stat_TOP
		  %12 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %13 = load x86_fp80, x86_fp80* @st6
		  %14 = add i3 %12, 1
		  store i3 %14, i3* @fpu_stat_TOP
		  br label %dec_label_end_branch
		dec_label_end_branch:
		  %15 = load i3, i3* @fpu_stat_TOP
		  %16 = load x86_fp80, x86_fp80* @st7
		  %17 = add i3 %15, 1
		  store i3 %17, i3* @fpu_stat_TOP
		  %18 = load i3, i3* @fpu_stat_TOP
		  %19 = load x86_fp80, x86_fp80* @st0
		  ret void
})";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // if_else_branch

TEST_F(X87FpuAnalysisTests, if_elseif_else_branch_or_switch)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			br i1 1, label %dec_label_if_then_true, label %dec_label_if_then_false
		dec_label_if_then_true:
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3);
			%5 = sub i3 %3, 1
			store i3 %5, i3* @fpu_stat_TOP
			%6 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %6, x86_fp80 0xK3FFF8000000000000000)
			%7 = call x86_fp80 @__frontend_reg_load.fpr(i3 %6);
			%8 = add i3 %6, 1
			store i3 %8, i3* @fpu_stat_TOP
			br label %dec_label_end_branch
		dec_label_if_then_false:
			br i1 1, label %dec_label_else_if_true, label %dec_label_else_if_false
		dec_label_else_if_true:
			%9 = load i3, i3* @fpu_stat_TOP
			%10 = call x86_fp80 @__frontend_reg_load.fpr(i3 %9);
			%11 = sub i3 %9, 1
			store i3 %11, i3* @fpu_stat_TOP
			%12 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %12, x86_fp80 0xK3FFF8000000000000000)
			%13 = call x86_fp80 @__frontend_reg_load.fpr(i3 %12);
			%14 = add i3 %12, 1
			store i3 %14, i3* @fpu_stat_TOP
			br label %dec_label_end_branch
		dec_label_else_if_false:
			br label %dec_label_end_branch
		dec_label_end_branch:
			%15 = load i3, i3* @fpu_stat_TOP
			%16 = call x86_fp80 @__frontend_reg_load.fpr(i3 %15);
			%17 = add i3 %15, 1
			store i3 %17, i3* @fpu_stat_TOP
			%18 = load i3, i3* @fpu_stat_TOP
			%19 = call x86_fp80 @__frontend_reg_load.fpr(i3 %18);
			ret void
		}
)");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define void @foo() {
		bb:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  br i1 true, label %dec_label_if_then_true, label %dec_label_if_then_false
		dec_label_if_then_true:
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st7
		  %5 = sub i3 %3, 1
		  store i3 %5, i3* @fpu_stat_TOP
		  %6 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %7 = load x86_fp80, x86_fp80* @st6
		  %8 = add i3 %6, 1
		  store i3 %8, i3* @fpu_stat_TOP
		  br label %dec_label_end_branch
		dec_label_if_then_false:
		  br i1 true, label %dec_label_else_if_true, label %dec_label_else_if_false
		dec_label_else_if_true:
		  %9 = load i3, i3* @fpu_stat_TOP
		  %10 = load x86_fp80, x86_fp80* @st7
		  %11 = sub i3 %9, 1
		  store i3 %11, i3* @fpu_stat_TOP
		  %12 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %13 = load x86_fp80, x86_fp80* @st6
		  %14 = add i3 %12, 1
		  store i3 %14, i3* @fpu_stat_TOP
		  br label %dec_label_end_branch
		dec_label_else_if_false:
		  br label %dec_label_end_branch
		dec_label_end_branch:
		  %15 = load i3, i3* @fpu_stat_TOP
		  %16 = load x86_fp80, x86_fp80* @st7
		  %17 = add i3 %15, 1
		  store i3 %17, i3* @fpu_stat_TOP
		  %18 = load i3, i3* @fpu_stat_TOP
		  %19 = load x86_fp80, x86_fp80* @st0
		  ret void
		}
)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // if_elseif_else_branch

TEST_F(X87FpuAnalysisTests, nested_branch_0)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
	define void @foo() {
		A:
			br i1 1, label %B, label %C
		B:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			br i1 1, label %D, label %E
		D:
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = call x86_fp80 @__frontend_reg_load.fpr(i3 %3)
			br label %E
		E:
			%5 = load i3, i3* @fpu_stat_TOP
			%6 = add i3 %5, 1
			store i3 %6, i3* @fpu_stat_TOP
			%7 = load i3, i3* @fpu_stat_TOP
			%8 = call x86_fp80 @__frontend_reg_load.fpr(i3 %7)
			br label %C
		C:
		ret void
	}
)");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
	define void @foo() {
		A:
		  br i1 true, label %B, label %C
		B:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  br i1 true, label %D, label %E
		D:
		  %3 = load i3, i3* @fpu_stat_TOP
		  %4 = load x86_fp80, x86_fp80* @st7
		  br label %E
		E:
		  %5 = load i3, i3* @fpu_stat_TOP
		  %6 = add i3 %5, 1
		  store i3 %6, i3* @fpu_stat_TOP
		  %7 = load i3, i3* @fpu_stat_TOP
		  %8 = load x86_fp80, x86_fp80* @st0
		  br label %C
		C:
		  ret void
	}
)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // nested_branch_0

TEST_F(X87FpuAnalysisTests, nested_branch_1)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
	define void @foo() {
		A:
			br i1 1, label %B, label %C
		B:
			%0 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %0, x86_fp80 0xK3FFF8000000000000000)
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			%3 = sub i3 %2, 1
			store i3 %3, i3* @fpu_stat_TOP
			%4 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %4, x86_fp80 0xK3FFF8000000000000000)
			%5 = sub i3 %4, 1
			store i3 %5, i3* @fpu_stat_TOP
			%6 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %6, x86_fp80 0xK3FFF8000000000000000)
			br i1 1, label %D, label %E
		D:
			%7 = load i3, i3* @fpu_stat_TOP
			%8 = call x86_fp80 @__frontend_reg_load.fpr(i3 %7)
			br label %E
		E:
			%9 = load i3, i3* @fpu_stat_TOP
			%10 = call x86_fp80 @__frontend_reg_load.fpr(i3 %9)
			%11 = add i3 %9, 1
			store i3 %11, i3* @fpu_stat_TOP
			%12 = load i3, i3* @fpu_stat_TOP
			%13 = add i3 %12, 1
			%14 = add i3 %13, 1
			store i3 %14, i3* @fpu_stat_TOP
			%15 = load i3, i3* @fpu_stat_TOP
			%16 = call x86_fp80 @__frontend_reg_load.fpr(i3 %15)
			br label %C
		C:
		ret void
	}
)");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	std::string exp = PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
	define void @foo() {
		A:
		  br i1 true, label %B, label %C
		B:
		  %0 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st0
		  %1 = sub i3 %0, 1
		  store i3 %1, i3* @fpu_stat_TOP
		  %2 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st7
		  %3 = sub i3 %2, 1
		  store i3 %3, i3* @fpu_stat_TOP
		  %4 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st6
		  %5 = sub i3 %4, 1
		  store i3 %5, i3* @fpu_stat_TOP
		  %6 = load i3, i3* @fpu_stat_TOP
		  store x86_fp80 0xK3FFF8000000000000000, x86_fp80* @st5
		  br i1 true, label %D, label %E
		D:
		  %7 = load i3, i3* @fpu_stat_TOP
		  %8 = load x86_fp80, x86_fp80* @st5
		  br label %E
		E:
		  %9 = load i3, i3* @fpu_stat_TOP
		  %10 = load x86_fp80, x86_fp80* @st5
		  %11 = add i3 %9, 1
		  store i3 %11, i3* @fpu_stat_TOP
		  %12 = load i3, i3* @fpu_stat_TOP
		  %13 = add i3 %12, 1
		  %14 = add i3 %13, 1
		  store i3 %14, i3* @fpu_stat_TOP
		  %15 = load i3, i3* @fpu_stat_TOP
		  %16 = load x86_fp80, x86_fp80* @st0
		  br label %C
		C:
		  ret void
	}
)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
} // nested_branch_1

TEST_F(X87FpuAnalysisTests, if_else_branch_fail)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
	define void @foo() {
	bb:
		br i1 1, label %dec_label_if_true, label %dec_label_if_false
		dec_label_if_true:
			%0 = load i3, i3* @fpu_stat_TOP
			%1 = sub i3 %0, 1
			store i3 %1, i3* @fpu_stat_TOP
			%2 = load i3, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %2, x86_fp80 0xK3FFF8000000000000000)
			br label %dec_label_end_branch
		dec_label_if_false:
			%3 = load i3, i3* @fpu_stat_TOP
			%4 = add i3 %3, 1
			store i3 %4, i3* @fpu_stat_TOP
			br label %dec_label_end_branch
		dec_label_end_branch:
		ret void
	}
)");

	setX86Environment("64", "unknown");
	bool b = pass.runOnModuleCustom(*module, config, abi);

	EXPECT_FALSE(b);
	unsigned pseudoCalls = 0;
	unsigned stackRegisterLoads = 0;
	unsigned stackRegisterStores = 0;
	std::set<Value*> stackRegisters;
	for (unsigned n = 0; n < 8; ++n)
	{
		stackRegisters.insert(getGlobalByName("st" + std::to_string(n)));
	}
	for (auto& instruction : instructions(getFunctionByName("foo")))
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataStorePseudoFunction();
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataLoadPseudoFunction();
		}
		else if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			stackRegisterLoads += stackRegisters.count(load->getPointerOperand());
		}
		else if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			stackRegisterStores += stackRegisters.count(store->getPointerOperand());
		}
	}
	EXPECT_EQ(0u, pseudoCalls);
	EXPECT_EQ(8u, stackRegisterLoads);
	EXPECT_EQ(8u, stackRegisterStores);
	EXPECT_FALSE(verifyModule(*module, &errs()));
} // if_else_branch_fail

TEST_F(X87FpuAnalysisTests, usesRuntimeTopForValueLoadedAfterCall)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		declare void @callee()
		define x86_fp80 @foo(x86_fp80 %value) {
		entry:
			%top.before = load i3, i3* @fpu_stat_TOP
			%pushed = sub i3 %top.before, 1
			store i3 %pushed, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %pushed, x86_fp80 %value)
			call void @callee()
			%top.after = load i3, i3* @fpu_stat_TOP
			%result = call x86_fp80 @__frontend_reg_load.fpr(i3 %top.after)
			ret x86_fp80 %result
		}
	)");

	setX86Environment("32", "cdecl");
	ASSERT_TRUE(pass.runOnModuleCustom(*module, config, abi));

	std::set<Value*> stackRegisters;
	for (unsigned n = 0; n < 8; ++n)
	{
		stackRegisters.insert(getGlobalByName("st" + std::to_string(n)));
	}

	auto* function = getFunctionByName("foo");
	auto* topAfter = getValueByName("top.after");
	unsigned runtimeStackLoads = 0;
	unsigned pseudoCalls = 0;
	for (auto& instruction : instructions(function))
	{
		if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			runtimeStackLoads += stackRegisters.count(load->getPointerOperand());
		}
		else if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataStorePseudoFunction();
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataLoadPseudoFunction();
		}
	}

	// The live-stack call makes both the store before the call and the load
	// after it use the architectural runtime TOP.
	EXPECT_EQ(16u, runtimeStackLoads);
	EXPECT_EQ(0u, pseudoCalls);
	auto* returned = cast<ReturnInst>(function->back().getTerminator())->getReturnValue();
	auto* resultSelect = dyn_cast<SelectInst>(returned);
	ASSERT_NE(nullptr, resultSelect);
	auto* condition = dyn_cast<ICmpInst>(resultSelect->getCondition());
	ASSERT_NE(nullptr, condition);
	EXPECT_EQ(topAfter, condition->getOperand(0));
}

TEST_F(X87FpuAnalysisTests, usesRuntimeTopForCalleeEnteredWithLiveStack)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define i32 @foo(x86_fp80 %value) {
		entry:
			%top.before = load i3, i3* @fpu_stat_TOP
			%pushed = sub i3 %top.before, 1
			store i3 %pushed, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %pushed, x86_fp80 %value)
			%result = call i32 @boo()
			ret i32 %result
		}
		define i32 @boo() {
		entry:
			%top.entry = load i3, i3* @fpu_stat_TOP
			%value = call x86_fp80 @__frontend_reg_load.fpr(i3 %top.entry)
			%result = fptosi x86_fp80 %value to i32
			ret i32 %result
		}
	)");

	setX86Environment("32", "cdecl");
	ASSERT_TRUE(pass.runOnModuleCustom(*module, config, abi));

	std::set<Value*> stackRegisters;
	for (unsigned n = 0; n < 8; ++n)
	{
		stackRegisters.insert(getGlobalByName("st" + std::to_string(n)));
	}

	auto* callee = getFunctionByName("boo");
	unsigned runtimeStackLoads = 0;
	unsigned pseudoCalls = 0;
	for (auto& instruction : instructions(callee))
	{
		if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			runtimeStackLoads += stackRegisters.count(load->getPointerOperand());
		}
		else if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataLoadPseudoFunction();
		}
	}

	EXPECT_EQ(8u, runtimeStackLoads);
	EXPECT_EQ(0u, pseudoCalls);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(X87FpuAnalysisTests,
		usesRuntimeTopAndBalancesTwoSequentialLiveStackCalls)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define i3 @foo(x86_fp80 %first, x86_fp80 %second) {
		entry:
			%top.before.first = load i3, i3* @fpu_stat_TOP
			%pushed.first = sub i3 %top.before.first, 1
			store i3 %pushed.first, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %pushed.first, x86_fp80 %first)
			%first.result = call i32 @boo()
			%top.after.first = load i3, i3* @fpu_stat_TOP
			%pushed.second = sub i3 %top.after.first, 1
			store i3 %pushed.second, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %pushed.second, x86_fp80 %second)
			%second.result = call i32 @boo()
			%top.after.second = load i3, i3* @fpu_stat_TOP
			ret i3 %top.after.second
		}
		define i32 @boo() {
		entry:
			%top.entry = load i3, i3* @fpu_stat_TOP
			%value = call x86_fp80 @__frontend_reg_load.fpr(i3 %top.entry)
			%result = fptosi x86_fp80 %value to i32
			%popped = add i3 %top.entry, 1
			store i3 %popped, i3* @fpu_stat_TOP
			ret i32 %result
		}
	)");

	setX86Environment("32", "cdecl");
	ASSERT_TRUE(pass.runOnModuleCustom(*module, config, abi));

	auto* caller = getFunctionByName("foo");
	auto* firstIndex = getValueByName("pushed.first");
	auto* secondIndex = getValueByName("pushed.second");
	unsigned firstSelections = 0;
	unsigned secondSelections = 0;
	for (auto& instruction : instructions(caller))
	{
		auto* compare = dyn_cast<ICmpInst>(&instruction);
		if (compare == nullptr)
		{
			continue;
		}
		firstSelections += compare->getOperand(0) == firstIndex;
		secondSelections += compare->getOperand(0) == secondIndex;
	}
	EXPECT_EQ(8u, firstSelections);
	EXPECT_EQ(8u, secondSelections);

	InstructionRdaOptimizer rda;
	rda.runOnModuleCustom(*module, abi);

	auto* returned = cast<ReturnInst>(caller->back().getTerminator())->getReturnValue();
	auto* finalTopLoad = dyn_cast<LoadInst>(returned);
	ASSERT_NE(nullptr, finalTopLoad);
	EXPECT_EQ(getGlobalByName("fpu_stat_TOP"), finalTopLoad->getPointerOperand());
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(X87FpuAnalysisTests, handlesIndirectCallBeforeX87Load)
{
	parseInput(PREDEFINED_REGISTERS_AND_FUNCTIONS + R"(
		define x86_fp80 @foo(void ()* %callee, x86_fp80 %value) {
		entry:
			%top.before = load i3, i3* @fpu_stat_TOP
			%pushed = sub i3 %top.before, 1
			store i3 %pushed, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %pushed, x86_fp80 %value)
			call void %callee()
			%top.after = load i3, i3* @fpu_stat_TOP
			%result = call x86_fp80 @__frontend_reg_load.fpr(i3 %top.after)
			ret x86_fp80 %result
		}
	)");

	setX86Environment("32", "cdecl");
	ASSERT_TRUE(pass.runOnModuleCustom(*module, config, abi));

	auto* function = getFunctionByName("foo");
	auto* topAfter = getValueByName("top.after");
	unsigned pseudoCalls = 0;
	for (auto& instruction : instructions(function))
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataStorePseudoFunction();
			pseudoCalls += call->getCalledFunction()
					== config->getLlvmX87DataLoadPseudoFunction();
		}
	}

	EXPECT_EQ(0u, pseudoCalls);
	auto* returned = cast<ReturnInst>(function->back().getTerminator())->getReturnValue();
	auto* resultSelect = dyn_cast<SelectInst>(returned);
	ASSERT_NE(nullptr, resultSelect);
	auto* condition = dyn_cast<ICmpInst>(resultSelect->getCondition());
	ASSERT_NE(nullptr, condition);
	EXPECT_EQ(topAfter, condition->getOperand(0));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
