execute_process(
	COMMAND "${RETDEC_DECOMPILER}" --pe32-pointer-bridge --help
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE error
)

if(NOT result EQUAL 0)
	message(FATAL_ERROR
		"retdec-decompiler rejected --pe32-pointer-bridge: ${error}")
endif()

foreach(required
		"--pe32-pointer-bridge"
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
