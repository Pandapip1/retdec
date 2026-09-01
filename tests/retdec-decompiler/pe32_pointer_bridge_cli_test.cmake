execute_process(
	COMMAND "${RETDEC_DECOMPILER}" --pe32-pointer-bridge --emit-lifted-dwarf --help
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE error
)

if(NOT result EQUAL 0)
	message(FATAL_ERROR
		"retdec-decompiler rejected --pe32-pointer-bridge: ${error}")
endif()

file(STRINGS "${RETDEC_DEFAULT_CONFIG}" type_info_paths
	REGEX "support/types/.*\\.json")
list(LENGTH type_info_paths type_info_path_count)
if(NOT type_info_path_count EQUAL 5)
	message(FATAL_ERROR
		"build-tree config does not contain five source LTI paths: ${type_info_paths}")
endif()
foreach(type_info_path_line IN LISTS type_info_paths)
	string(REGEX REPLACE "^[^\"]*\"([^\"]+)\".*$" "\\1"
		type_info_path "${type_info_path_line}")
	if(NOT EXISTS "${type_info_path}")
		message(FATAL_ERROR
			"build-tree config references missing LTI data: ${type_info_path}")
	endif()
endforeach()

foreach(required
		"--pe32-pointer-bridge"
		"--emit-lifted-dwarf"
		"--analysis-metadata"
		"--stop-after analysis|bin2llvmir"
		"__retdec_pe32_host_to_guest"
		"__retdec_pe32_guest_to_host")
	string(FIND "${output}" "${required}" found)
	if(found EQUAL -1)
		message(FATAL_ERROR
			"retdec-decompiler help omits required text: ${required}")
	endif()
endforeach()

execute_process(
	COMMAND "${RETDEC_DECOMPILER}" --definitely-unknown-option
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE error
)
if(result EQUAL 0 OR NOT error MATCHES "unknown option")
	message(FATAL_ERROR
		"retdec-decompiler accepted an unknown option: ${output}${error}")
endif()

set(custom_config "${CMAKE_CURRENT_BINARY_DIR}/custom-decompiler-config.json")
file(WRITE "${custom_config}" [=[
{
  "decompParams": {
    "inputFile": "configured-input.bin",
    "llvmPasses": ["retdec-pe32-pointer-cells"]
  }
}
]=])
execute_process(
	COMMAND "${RETDEC_DECOMPILER}"
		--config "${custom_config}" command-line-input.bin
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE error
)
if(result EQUAL 0
		OR NOT error MATCHES
			"unexpected positional argument: command-line-input.bin")
	message(FATAL_ERROR
		"--config parameters were not retained: ${output}${error}")
endif()
