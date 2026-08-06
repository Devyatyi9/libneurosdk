cmake_minimum_required(VERSION 3.30)

project(neurosdk VERSION 0.3.0)

include(GNUInstallDirs)

option(NEURO_BUILD_STATIC "Build the library statically" ON)
option(NEURO_BUILD_TESTS "Build test executables" OFF)
option(NEURO_BUILD_EXAMPLES "Build example executables" OFF)
option(NEURO_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(NEURO_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
option(ENABLE_PERMESSAGE_DEFLATE "Build the internal RFC 7692 codec" ON)

if(NEURO_SANITIZE)
	if(MSVC)
		add_compile_options(/fsanitize=address /Zi)
		add_link_options(/DEBUG)
		if(CMAKE_SIZEOF_VOID_P EQUAL 8)
			message(STATUS "MSVC ASan (x64): add Hostx64/x64 directory to PATH at runtime")
		else()
			message(STATUS "MSVC ASan (x86): add Hostx64/x86 directory to PATH at runtime")
		endif()
	else()
		add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g)
		add_link_options(-fsanitize=address -fsanitize=undefined)
	endif()
endif()

if(MSVC)
	add_compile_options(/W4 /permissive- /std:c11)
	if(NEURO_WARNINGS_AS_ERRORS)
		add_compile_options(/WX)
	endif()
else()
	add_compile_options(-Wall -Wextra -Wpedantic -Wimplicit-fallthrough -std=gnu11)
	if(NEURO_WARNINGS_AS_ERRORS)
		add_compile_options(-Werror)
	endif()
endif()

execute_process(
	COMMAND git rev-parse HEAD
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	OUTPUT_VARIABLE LIB_BUILD_HASH
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)
if(NOT LIB_BUILD_HASH)
	set(LIB_BUILD_HASH "unknown")
endif()

set(SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/neurosdk.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/protocol_json.c
	${CMAKE_CURRENT_SOURCE_DIR}/src/ws_client.c
	${CMAKE_CURRENT_SOURCE_DIR}/vendor/tinycthread.c
)
if(MSVC)
	set_source_files_properties(vendor/tinycthread.c PROPERTIES COMPILE_OPTIONS /wd5105)
endif()

if(ENABLE_PERMESSAGE_DEFLATE)
	# miniz.h includes miniz_zip.h even when archive APIs are disabled. Supply an
	# empty build-tree shim so no ZIP header or implementation is vendored.
	file(GENERATE
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/miniz-private/miniz_zip.h
		CONTENT "#pragma once\n"
	)
	add_library(neuro_miniz OBJECT
		vendor/miniz/miniz.c
		vendor/miniz/miniz_tdef.c
		vendor/miniz/miniz_tinfl.c
	)
	set_target_properties(neuro_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
	target_include_directories(neuro_miniz PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniz
		${CMAKE_CURRENT_BINARY_DIR}/miniz-private
	)
	target_compile_definitions(neuro_miniz PRIVATE
		MINIZ_NO_ARCHIVE_APIS
		MINIZ_NO_STDIO
		MINIZ_NO_TIME
		MINIZ_NO_ZLIB_COMPATIBLE_NAMES
	)
	if(MSVC)
		target_compile_options(neuro_miniz PRIVATE /W0)
	else()
		target_compile_options(neuro_miniz PRIVATE -w)
	endif()

	add_library(ws_deflate OBJECT src/ws_deflate.c)
	set_target_properties(ws_deflate PROPERTIES POSITION_INDEPENDENT_CODE ON)
	target_include_directories(ws_deflate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_include_directories(ws_deflate PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniz
		${CMAKE_CURRENT_BINARY_DIR}/miniz-private
	)
endif()

if(NEURO_BUILD_STATIC)
	add_library(${PROJECT_NAME} STATIC ${SOURCES})
else()
	add_library(${PROJECT_NAME} SHARED ${SOURCES})
endif()
if(ENABLE_PERMESSAGE_DEFLATE)
	target_sources(${PROJECT_NAME} PRIVATE
		$<TARGET_OBJECTS:ws_deflate>
		$<TARGET_OBJECTS:neuro_miniz>
	)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/vendor
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
	LIB_VERSION=${PROJECT_VERSION}
	LIB_BUILD_HASH=${LIB_BUILD_HASH}
	$<$<BOOL:${ENABLE_PERMESSAGE_DEFLATE}>:WS_ENABLE_PERMESSAGE_DEFLATE>
)

if(WIN32)
	target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 bcrypt)
endif()
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/neurosdk.pc.in
	${CMAKE_CURRENT_BINARY_DIR}/neurosdk.pc
	@ONLY
)

install(TARGETS ${PROJECT_NAME}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/neurosdk.h
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(NEURO_BUILD_TESTS)
	add_library(ws_client_test STATIC src/ws_client.c)
	target_include_directories(ws_client_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
	if(ENABLE_PERMESSAGE_DEFLATE)
		target_compile_definitions(ws_client_test PRIVATE WS_ENABLE_PERMESSAGE_DEFLATE)
		target_sources(ws_client_test PRIVATE
			$<TARGET_OBJECTS:ws_deflate>
			$<TARGET_OBJECTS:neuro_miniz>
		)
	endif()
	if(WIN32)
		target_link_libraries(ws_client_test PUBLIC ws2_32 bcrypt)
	endif()

	add_executable(test_s2c_action
		tests/protocol/test_s2c_action.c
		src/protocol_json.c
		vendor/tinycthread.c
	)
	target_include_directories(test_s2c_action PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/vendor
		${CMAKE_CURRENT_SOURCE_DIR}/src
	)
	# test_s2c_action includes neurosdk.c directly to exercise internal parsing.
	target_compile_definitions(test_s2c_action PRIVATE
		LIB_VERSION=${PROJECT_VERSION}
		LIB_BUILD_HASH=${LIB_BUILD_HASH}
	)
	target_link_libraries(test_s2c_action PRIVATE ws_client_test)
	add_executable(test_c2s_json tests/protocol/test_c2s_json.c)
	target_include_directories(test_c2s_json PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/src
		${CMAKE_CURRENT_SOURCE_DIR}/vendor)
	target_link_libraries(test_c2s_json PRIVATE ws_client_test)
	add_executable(protocol_interop_client tests/protocol/interop_client.c)
	target_link_libraries(protocol_interop_client PRIVATE ${PROJECT_NAME})
	add_custom_target(protocol_test_suite DEPENDS
		test_s2c_action test_c2s_json protocol_interop_client)

	add_executable(echo_test tests/websocket/integration/echo_test.c)
	target_link_libraries(echo_test PRIVATE ws_client_test)

	add_executable(integration_test tests/websocket/integration/integration_test.c)
	target_link_libraries(integration_test PRIVATE ws_client_test)

	add_executable(fuzzing_client tests/websocket/autobahn/fuzzing_client.c)
	target_link_libraries(fuzzing_client PRIVATE ws_client_test)
	
	add_executable(long_session tests/websocket/perf/long_session.c)
	target_link_libraries(long_session PRIVATE ws_client_test)

	# Unit tests (include ws_client.c directly for static function access)
	set(UNIT_TESTS test_url test_sha1 test_base64 test_frame)
	if(ENABLE_PERMESSAGE_DEFLATE)
		list(APPEND UNIT_TESTS test_ws_deflate)
	endif()
	foreach(t ${UNIT_TESTS})
		add_executable(${t} tests/websocket/unit/${t}.c)
		target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
		if(t STREQUAL "test_ws_deflate")
			target_sources(${t} PRIVATE
				$<TARGET_OBJECTS:ws_deflate>
				$<TARGET_OBJECTS:neuro_miniz>
			)
		endif()
		if(ENABLE_PERMESSAGE_DEFLATE AND NOT t STREQUAL "test_ws_deflate")
			target_compile_definitions(${t} PRIVATE WS_ENABLE_PERMESSAGE_DEFLATE)
			target_sources(${t} PRIVATE $<TARGET_OBJECTS:ws_deflate>
				$<TARGET_OBJECTS:neuro_miniz>)
		endif()
		if(WIN32)
			target_link_libraries(${t} PRIVATE ws2_32 bcrypt)
		endif()
	endforeach()

	# Network I/O test clients exercise ws_client directly.
	set(NET_TESTS test_partial_send test_flood test_double_close test_oversize proxy_test test_large_frame test_peer_close test_negative test_pmd)
	foreach(t ${NET_TESTS})
		add_executable(${t} tests/websocket/network/${t}.c)
		target_link_libraries(${t} PRIVATE ws_client_test)
	endforeach()

	add_custom_target(ws_test_suite DEPENDS
		echo_test integration_test fuzzing_client long_session
		${UNIT_TESTS} ${NET_TESTS}
	)
endif()

if(NEURO_BUILD_EXAMPLES)
	add_executable(simple_example examples/simple.c)
	target_include_directories(simple_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(simple_example PRIVATE ${PROJECT_NAME})

	add_executable(tictactoe_example examples/tictactoe.c)
	target_include_directories(tictactoe_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(tictactoe_example PRIVATE ${PROJECT_NAME})

	add_executable(reconnect_example examples/reconnect.c)
	target_include_directories(reconnect_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(reconnect_example PRIVATE ${PROJECT_NAME})

	add_executable(rock_paper_scissors_example examples/rock_paper_scissors.c)
	target_include_directories(rock_paper_scissors_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_link_libraries(rock_paper_scissors_example PRIVATE ${PROJECT_NAME})
	if(TARGET protocol_test_suite)
		add_dependencies(protocol_test_suite
			simple_example tictactoe_example reconnect_example
			rock_paper_scissors_example)
	endif()
endif()

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neurosdk.pc
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
