option(ZAP_BUILD_REFERENCE "Build ZAP Doxygen Reference" ON)

if(ZAP_BUILD_REFERENCE)
    find_package(Doxygen)
    if(Doxygen_FOUND)
        message(STATUS "Found Doxygen Version "
            "${DOXYGEN_VERSION} at ${DOXYGEN_EXECUTABLE}")

        set(ZAP_DOXYGEN_IN  ${CMAKE_CURRENT_SOURCE_DIR}/Doxygen.in)
        set(ZAP_DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

        configure_file(${ZAP_DOXYGEN_IN} ${ZAP_DOXYGEN_OUT} @ONLY)

        message("Building Doxygen")

        add_custom_target(zap_doxygen ALL
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating reference with Doxygen"
            VERBATIM
        )
    endif()
endif()