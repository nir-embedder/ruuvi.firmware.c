cmake_minimum_required(VERSION 3.20)
project(ruuvi_format_parity C)

set(ENDPOINTS "${CMAKE_CURRENT_LIST_DIR}/../../nrf52_oldsdk/src/ruuvi.endpoints.c/src")
add_executable(test_df5
    test_df5.c
    "${ENDPOINTS}/ruuvi_endpoint_5.c"
    "${ENDPOINTS}/ruuvi_endpoints.c")
target_include_directories(test_df5 PRIVATE "${ENDPOINTS}")
target_link_libraries(test_df5 PRIVATE m)

add_executable(test_formats
    test_formats.c
    ../src/formats.c
    "${ENDPOINTS}/ruuvi_endpoint_3.c"
    "${ENDPOINTS}/ruuvi_endpoint_5.c"
    "${ENDPOINTS}/ruuvi_endpoint_7.c"
    "${ENDPOINTS}/ruuvi_endpoint_8.c"
    "${ENDPOINTS}/ruuvi_endpoint_c5.c"
    "${ENDPOINTS}/ruuvi_endpoint_fa.c"
    "${ENDPOINTS}/ruuvi_endpoints.c")
target_include_directories(test_formats PRIVATE ../src "${ENDPOINTS}")
target_link_libraries(test_formats PRIVATE m)

add_executable(test_history_codec test_history_codec.c ../src/history_codec.c)
target_include_directories(test_history_codec PRIVATE ../src)

add_executable(test_history test_history.c ../src/history.c ../src/history_codec.c)
target_include_directories(test_history PRIVATE ../src)

add_executable(test_log_protocol test_log_protocol.c ../src/log_protocol.c
    "${ENDPOINTS}/ruuvi_endpoints.c")
target_include_directories(test_log_protocol PRIVATE ../src "${ENDPOINTS}")
target_link_libraries(test_log_protocol PRIVATE m)

add_executable(test_log_service test_log_service.c ../src/log_service.c
    ../src/log_protocol.c "${ENDPOINTS}/ruuvi_endpoints.c")
target_include_directories(test_log_service PRIVATE ../src "${ENDPOINTS}")
target_link_libraries(test_log_service PRIVATE m)

add_executable(test_nfc_codec test_nfc_codec.c ../src/nfc_codec.c)
target_include_directories(test_nfc_codec PRIVATE ../src)

enable_testing()
add_test(NAME df5_legacy_vectors COMMAND test_df5)
add_test(NAME formats_legacy_vectors_and_rotation COMMAND test_formats)
add_test(NAME history_fixed_width_records COMMAND test_history_codec)
add_test(NAME history_rotation_and_reads COMMAND test_history)
add_test(NAME log_read_wire_frames COMMAND test_log_protocol)
add_test(NAME log_stream_backpressure_and_auth COMMAND test_log_service)
add_test(NAME nfc_ndef_legacy_vectors_and_validation COMMAND test_nfc_codec)

find_path(MBEDTLS_PSA_INCLUDE psa/crypto.h
    HINTS /opt/homebrew/opt/mbedtls@3/include)
find_library(MBEDCRYPTO_LIB NAMES mbedcrypto
    HINTS /opt/homebrew/opt/mbedtls@3/lib)
if(MBEDTLS_PSA_INCLUDE AND MBEDCRYPTO_LIB)
    add_executable(test_aes test_aes.c ../src/crypto.c)
    target_include_directories(test_aes PRIVATE ../src "${MBEDTLS_PSA_INCLUDE}")
    target_link_libraries(test_aes PRIVATE "${MBEDCRYPTO_LIB}")
    add_test(NAME aes128_ecb_nist_vector COMMAND test_aes)
endif()
