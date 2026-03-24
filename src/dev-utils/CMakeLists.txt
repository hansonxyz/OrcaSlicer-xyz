
option(SLIC3R_ENC_CHECK "Verify encoding of source files" 1)

if (IS_CROSS_COMPILE)
    # Force disable due to cross compilation. This fact is already printed on cli for users
    set(SLIC3R_ENC_CHECK OFF CACHE BOOL "" FORCE)
endif ()

if (SLIC3R_ENC_CHECK)
    add_executable(encoding-check encoding-check.cpp)

    # A global no-op target which depends on all encodings checks,
    # and on which in turn all checked targets depend.
    # This is done to make encoding checks the first thing to be
    # performed before actually compiling any sources of the checked targets
    # to make the check fail as early as possible.
    add_custom_target(global-encoding-check
        ALL
        DEPENDS encoding-check
    )
endif()

# Function that adds source file encoding check to a target
# using the above encoding-check binary.
# Uses a response file to batch all source files into a single invocation,
# avoiding per-file process spawning overhead (~30s savings on incremental builds).

function(encoding_check TARGET)
    if (SLIC3R_ENC_CHECK)
        # Obtain target source files
        get_target_property(T_SOURCES ${TARGET} SOURCES)

        # Build a response file with absolute paths to all source files
        get_target_property(T_SOURCE_DIR ${TARGET} SOURCE_DIR)
        set(RSP_FILE "${CMAKE_CURRENT_BINARY_DIR}/encoding-check-${TARGET}.rsp")
        set(STAMP_FILE "${CMAKE_CURRENT_BINARY_DIR}/encoding-check-${TARGET}.stamp")

        set(RSP_CONTENT "")
        set(ABS_SOURCES "")
        foreach(file ${T_SOURCES})
            if (IS_ABSOLUTE "${file}")
                set(abs_file "${file}")
            else()
                set(abs_file "${T_SOURCE_DIR}/${file}")
            endif()
            string(APPEND RSP_CONTENT "${abs_file}\n")
            list(APPEND ABS_SOURCES "${abs_file}")
        endforeach()
        file(WRITE "${RSP_FILE}" "${RSP_CONTENT}")

        # Single custom command that checks all files at once via response file
        add_custom_command(
            OUTPUT "${STAMP_FILE}"
            COMMAND $<TARGET_FILE:encoding-check> ${TARGET} "@${RSP_FILE}"
            COMMAND ${CMAKE_COMMAND} -E touch "${STAMP_FILE}"
            DEPENDS encoding-check ${ABS_SOURCES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Checking source file encodings for target ${TARGET}"
        )

        add_custom_target(encoding-check-${TARGET}
            DEPENDS "${STAMP_FILE}"
        )

        add_dependencies(global-encoding-check encoding-check-${TARGET})
        add_dependencies(${TARGET} global-encoding-check)
    endif()
endfunction()
