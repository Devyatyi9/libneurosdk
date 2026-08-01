cmake_minimum_required(VERSION 3.30)

project(neurosdk VERSION 0.2.1)

include(GNUInstallDirs)

option(NEURO_BUILD_STATIC "Build the library statically" ON)
option(NEURO_BUILD_TESTS "Build test executables" OFF)
option(NEURO_BUILD_EXAMPLES "Build example executables" OFF)
option(NEURO_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

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
else()
	add_compile_options(-Wall -Wextra -Wpedantic -Wimplicit-fallthrough -std=gnu11)
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
	${CMAKE_CURRENT_SOURCE_DIR}/src/ws_client.c
)

if(NEURO_BUILD_STATIC)
	add_library(${PROJECT_NAME} STATIC ${SOURCES})
else()
	add_library(${PROJECT_NAME} SHARED ${SOURCES})
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
	if(WIN32)
		target_link_libraries(ws_client_test PUBLIC ws2_32 bcrypt)
	endif()

	add_executable(echo_test tests/integration/echo_test.c)
	target_link_libraries(echo_test PRIVATE ws_client_test)

	add_executable(integration_test tests/integration/integration_test.c)
	target_link_libraries(integration_test PRIVATE ws_client_test)

	add_executable(fuzzing_client tests/autobahn/fuzzing_client.c)
	target_link_libraries(fuzzing_client PRIVATE ws_client_test)
	
	add_executable(long_session tests/perf/long_session.c)
	target_link_libraries(long_session PRIVATE ws_client_test)

	# Unit tests (include ws_client.c directly for static function access)
	set(UNIT_TESTS test_url test_sha1 test_base64 test_frame)
	foreach(t ${UNIT_TESTS})
		add_executable(${t} tests/unit/${t}.c)
		target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
		if(WIN32)
			target_link_libraries(${t} PRIVATE ws2_32 bcrypt)
		endif()
	endforeach()

	# Network I/O test clients exercise ws_client directly.
	set(NET_TESTS test_partial_send test_flood test_double_close test_oversize proxy_test test_large_frame test_peer_close test_negative)
	foreach(t ${NET_TESTS})
		add_executable(${t} tests/network/${t}.c)
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
endif()

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neurosdk.pc
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
