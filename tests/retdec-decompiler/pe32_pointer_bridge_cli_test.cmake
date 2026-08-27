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
