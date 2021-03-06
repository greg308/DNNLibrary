find_package(Protobuf)

# dnn_protobuf_generate_cpp(ONNX_PROTO_SRCS ONNX_PROTO_HDRS onnx.proto3)
# message(${ONNX_PROTO_SRCS})
# message(${CMAKE_CURRENT_BINARY_DIR})
add_executable(onnx2daq
    ${CMAKE_CURRENT_SOURCE_DIR}/onnx2daq.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OnnxConverter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OnnxConverter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/NodeAttrHelper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NodeAttrHelper.h
    ${PROJECT_SOURCE_DIR}/common/StrKeyMap.h
    ${PROJECT_SOURCE_DIR}/common/Shaper.h
    ${PROJECT_SOURCE_DIR}/common/Shaper.cpp
    ${ONNX_PROTO_SRCS}
    ${ONNX_PROTO_HDRS})

treat_warnings_as_errors(onnx2daq)

target_link_libraries(onnx2daq
    protobuf::libprotobuf
    glog::glog
    onnx)

target_include_directories(onnx2daq
    PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    )
