add_custom_target(tests-retdec-decompiler
	COMMAND
		${CMAKE_COMMAND}
		-DRETDEC_DECOMPILER=$<TARGET_FILE:retdec-decompiler>
		-P ${CMAKE_CURRENT_SOURCE_DIR}/pe32_pointer_bridge_cli_test.cmake
	DEPENDS
		retdec-decompiler
)
