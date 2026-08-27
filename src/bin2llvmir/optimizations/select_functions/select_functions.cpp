/**
 * @file src/bin2llvmir/optimizations/select_functions/select_functions.cpp
 * @brief If ranges or functions are selected in config, remove bodies of all
 *        functions that are not selected.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 */

#include <iomanip>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

#include "retdec/bin2llvmir/optimizations/select_functions/select_functions.h"
#include "retdec/bin2llvmir/utils/debug.h"
#define debug_enabled false

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char SelectFunctions::ID = 0;

static RegisterPass<SelectFunctions> X(
		"retdec-select-fncs",
		"Selected functions optimization",
		 false, // Only looks at CFG
		 false // Analysis Pass
);

SelectFunctions::SelectFunctions() :
		ModulePass(ID)
{

}

bool SelectFunctions::runOnModule(Module& M)
{
	_config = ConfigProvider::getConfig(&M);
	return run(M);
}

bool SelectFunctions::runOnModuleCustom(llvm::Module& M, Config* c)
{
	_config = c;
	return run(M);
}

bool SelectFunctions::run(Module& M)
{
	if (_config == nullptr)
	{
		return false;
	}
	if (!_config->getConfig().parameters.isSomethingSelected())
	{
		return false;
	}

	bool changed = false;

	LOG << "functions:" << std::endl;
	for (Function& f : M.getFunctionList())
	{
		if (f.isDeclaration())
		{
			continue;
		}

		auto* cf = _config->getConfigFunction(&f);
		if (cf == nullptr)
		{
			continue;
		}

		LOG << "\t" << f.getName().str() << ": " << cf->getStart()
				<< " -- " << cf->getEnd() << std::endl;

		if (_config->isFunctionSelected(cf))
		{
			_config->getConfig().parameters.selectedNotFoundFunctions.erase(
					f.getName());
			LOG << "\t\tselected -- keep" << std::endl;
			continue;
		}

		LOG << "\t\tdelete body" << std::endl;
		f.deleteBody();
		changed = true;
	}

	return changed;
}

} // namespace bin2llvmir
} // namespace retdec
