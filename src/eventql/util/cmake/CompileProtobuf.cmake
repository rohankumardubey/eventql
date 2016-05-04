#=============================================================================
# Copyright 2009 Kitware, Inc.
# Copyright 2009 Philip Lowman <philip@yhbt.com>
# Copyright 2008 Esben Mose Hansen, Ange Optimization ApS
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================
# (To distributed this file outside of CMake, substitute the full
#  License text for the above reference.)
cmake_policy(SET CMP0026 OLD)

function(STX_PROTOBUF_GENERATE_CPP SRCS HDRS)
  get_property(PROTOBUF_PROTOC_EXECUTABLE TARGET stx-protoc PROPERTY LOCATION)

  if(NOT ARGN)
    message(SEND_ERROR "Error: STX_PROTOBUF_GENERATE_CPP() called without any proto files")
    return()
  endif(NOT ARGN)

  set(${SRCS})
  set(${HDRS})
  foreach(FIL ${ARGN})
    get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
    get_filename_component(FIL_WE ${FIL} NAME_WE)

    get_filename_component(ROOT_DIR ${CMAKE_SOURCE_DIR}/src ABSOLUTE)
    string(LENGTH ${ROOT_DIR} ROOT_DIR_LEN)

    get_filename_component(FIL_DIR ${ABS_FIL} PATH)
    string(LENGTH ${FIL_DIR} FIL_DIR_LEN)
    math(EXPR FIL_DIR_REM "${FIL_DIR_LEN} - ${ROOT_DIR_LEN}")

    string(SUBSTRING ${FIL_DIR} ${ROOT_DIR_LEN} ${FIL_DIR_REM} FIL_DIR_PREFIX)
    set(FIL_WEPREFIX "${FIL_DIR_PREFIX}/${FIL_WE}")

    list(APPEND ${SRCS} "${CMAKE_BINARY_DIR}/${FIL_WEPREFIX}.pb.cc")
    list(APPEND ${HDRS} "${CMAKE_BINARY_DIR}/${FIL_WEPREFIX}.pb.h")

    add_custom_command(
      OUTPUT "${CMAKE_BINARY_DIR}/${FIL_WEPREFIX}.pb.cc"
             "${CMAKE_BINARY_DIR}/${FIL_WEPREFIX}.pb.h"
      COMMAND  ${PROTOBUF_PROTOC_EXECUTABLE}
      ARGS --cpp_out ${CMAKE_BINARY_DIR} --proto_path ${CMAKE_SOURCE_DIR}/src/eventql/util/3rdparty --proto_path ${CMAKE_SOURCE_DIR}/src ${ABS_FIL}
      DEPENDS ${ABS_FIL}
      VERBATIM )
  endforeach()

  set_source_files_properties(${${SRCS}} ${${HDRS}} PROPERTIES GENERATED TRUE)
  set(${SRCS} ${${SRCS}} PARENT_SCOPE)
  set(${HDRS} ${${HDRS}} PARENT_SCOPE)
endfunction()


