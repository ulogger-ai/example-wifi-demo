# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	example-wifi-demo
	VERSION 1.0
	LANGUAGES C CXX ASM
)

# Include the definition of the slc_example-wifi-demo target,
# which contains the content of the SLC project
include(example-wifi-demo.cmake)

# app.c is compiled INTO the slc object library, not into the executable, so an
# include path set on the executable target below never reaches it. The agent
# headers have to go on slc itself. PUBLIC, so the setting also propagates to
# the executable and covers the sources added to it further down.
target_include_directories(slc PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)

add_executable(example-wifi-demo
    # Add additional sources here
    # mqtt.c, logging.c and ulogger_modules_def.c are supplied by the managed
    # project file (example-wifi-demo_project.cmake, included at the bottom of
    # this file), so they are deliberately not repeated here. If they ever stop
    # being compiled, check that file first.
)

target_include_directories(example-wifi-demo PUBLIC
    # Add additional include paths here
    # Also set on the slc target above, which is where app.c is compiled.
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)

target_compile_definitions(example-wifi-demo PUBLIC
    # Add additional macros here
)

target_compile_options(example-wifi-demo PUBLIC
    # Set additional compiler flags here
)

target_link_options(example-wifi-demo PUBLIC
    # Set additional linker flags here
    # uLogger defines a strong HardFault_Handler inside its archive, but nothing
    # in the application references it -- without -u the member is never pulled
    # in, the vendor's default handler wins, and crash dumps are silently never
    # captured. There is no build error if this is missing.
    -Wl,-u,HardFault_Handler
)

# Link with the content defined in the SLC project
# The uLogger archive is listed before slc so its strong HardFault_Handler is
# resolved ahead of the weak definition in the vendor startup object.
target_link_libraries(example-wifi-demo PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../lib/libULogger_armv7m.a"
    slc
)

# Include managed project content if available
include(example-wifi-demo_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(example-wifi-demo PROPERTIES LINKER_LANGUAGE C)

# Create .bin, .hex and .s37 artifacts after building the project
add_custom_command(TARGET example-wifi-demo
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_SREC_CMD} "$<TARGET_FILE:example-wifi-demo>" "$<TARGET_FILE_DIR:example-wifi-demo>/$<TARGET_FILE_BASE_NAME:example-wifi-demo>.s37"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_IHEX_CMD} "$<TARGET_FILE:example-wifi-demo>" "$<TARGET_FILE_DIR:example-wifi-demo>/$<TARGET_FILE_BASE_NAME:example-wifi-demo>.hex"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_BIN_CMD}  "$<TARGET_FILE:example-wifi-demo>" "$<TARGET_FILE_DIR:example-wifi-demo>/$<TARGET_FILE_BASE_NAME:example-wifi-demo>.bin" 
)

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET example-wifi-demo
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()