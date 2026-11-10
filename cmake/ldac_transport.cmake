# Derived from esp32-s31-ldac-receiver. Patch generated copies only.
set(_ldac_script "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare_ldac_transport.py")
set(_ldac_header "${CMAKE_CURRENT_LIST_DIR}/../scripts/repo_ldac_transport.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ldac_script}" "${_ldac_header}"
    "${CMAKE_CURRENT_LIST_DIR}/../scripts/repo_bt_diag.h" "${CMAKE_CURRENT_LIST_DIR}/../scripts/repo_bt_diag.c")
set(_ldac_dir "${CMAKE_BINARY_DIR}/ldac_transport")
execute_process(COMMAND "${PYTHON}" "${_ldac_script}" --framework "${IDF_PATH}" --output "${_ldac_dir}"
                RESULT_VARIABLE _ldac_result OUTPUT_VARIABLE _ldac_out ERROR_VARIABLE _ldac_err)
if(NOT _ldac_result EQUAL 0)
    message(FATAL_ERROR "Cannot prepare LDAC source transport: ${_ldac_out}${_ldac_err}")
endif()
idf_component_get_property(_ldac_bt_lib bt COMPONENT_LIB)
# Both the API vendor union and every host-stack codec array must fit LDAC.
target_include_directories(${_ldac_bt_lib} BEFORE PUBLIC "${_ldac_dir}/include")
target_compile_definitions(${_ldac_bt_lib} PUBLIC AVDT_CODEC_SIZE=11)
# GCC 15 diagnoses intentional name truncation in the stock bt_types.h macro.
# Keep the diagnostic visible without making this SDK warning fail the build.
target_compile_options(${_ldac_bt_lib} PRIVATE -Wno-error=stringop-truncation)
get_target_property(_ldac_sources ${_ldac_bt_lib} SOURCES)
set(_ldac_patched "")
set(_ldac_count 0)
foreach(_source IN LISTS _ldac_sources)
    get_filename_component(_name "${_source}" NAME)
    if(_name STREQUAL "btc_av.c" OR _name STREQUAL "bta_av_co.c" OR
       _name STREQUAL "bta_av_aact.c" OR _name STREQUAL "btc_a2dp_source_ext_codec.c" OR
       _name STREQUAL "l2c_link.c" OR _name STREQUAL "avdt_msg.c")
        list(APPEND _ldac_patched "${_ldac_dir}/${_name}")
        math(EXPR _ldac_count "${_ldac_count} + 1")
    else()
        list(APPEND _ldac_patched "${_source}")
    endif()
endforeach()
if(NOT _ldac_count EQUAL 6)
    message(FATAL_ERROR "LDAC source overlay requires six SDK sources; found ${_ldac_count}")
endif()
list(APPEND _ldac_patched "${_ldac_dir}/repo_bt_diag.c")
set_property(TARGET ${_ldac_bt_lib} PROPERTY SOURCES "${_ldac_patched}")
message(STATUS "LDAC: project-local Bluedroid source transport enabled")
