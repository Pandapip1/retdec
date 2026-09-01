add_custom_target(tests-retdec-decompiler
	COMMAND
		${CMAKE_COMMAND}
		-DRETDEC_DECOMPILER=$<TARGET_FILE:retdec-decompiler>
		-DRETDEC_DEFAULT_CONFIG=${PROJECT_BINARY_DIR}/src/share/retdec/decompiler-config.json
		-P ${CMAKE_CURRENT_SOURCE_DIR}/pe32_pointer_bridge_cli_test.cmake
	DEPENDS
		retdec-decompiler
)
