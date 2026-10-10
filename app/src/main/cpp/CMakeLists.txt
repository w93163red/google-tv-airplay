cmake_minimum_required(VERSION 3.22)
project(airscreen C CXX)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party)
set(UX ${TP}/uxplay/lib)
set(PREBUILT ${TP}/prebuilt/${ANDROID_ABI})
if(NOT EXISTS ${PREBUILT}/lib/libcrypto.a)
    message(FATAL_ERROR "missing ${PREBUILT}/lib/libcrypto.a; run scripts/build-deps.sh")
endif()

file(GLOB UX_SRC ${UX}/*.c ${UX}/playfair/*.c ${UX}/llhttp/*.c)
file(GLOB PLIST_SRC ${TP}/libplist/src/*.c)
set(CNARY_SRC ${TP}/libplist/libcnary/node.c ${TP}/libplist/libcnary/node_list.c)
file(GLOB ALAC_SRC ${TP}/alac/*.c ${TP}/alac/*.cpp)
file(GLOB APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_library(crypto_static STATIC IMPORTED)
set_target_properties(crypto_static PROPERTIES IMPORTED_LOCATION ${PREBUILT}/lib/libcrypto.a)

add_library(airscreen SHARED ${APP_SRC} ${UX_SRC} ${PLIST_SRC} ${CNARY_SRC} ${ALAC_SRC})

target_include_directories(airscreen PRIVATE
    ${UX} ${UX}/playfair ${UX}/llhttp
    ${TP}/libplist/include ${TP}/libplist/src ${TP}/libplist/libcnary/include
    ${TP}/alac
    ${PREBUILT}/include)

# UxPlay's lib/CMakeLists.txt POSIX flags, plus what its pkg-config probes would define.
target_compile_definitions(airscreen PRIVATE
    STANDALONE __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS TARGET_POSIX _LINUX _REENTRANT
    NOHOLD PLIST_210 PLIST_230 OPENSSL_API_COMPAT=0x10101000L
    LIBPLIST_STATIC HAVE_STRNDUP HAVE_GMTIME_R HAVE_LOCALTIME_R HAVE_TIMEGM
    HAVE_TM_TM_GMTOFF HAVE_TM_TM_ZONE PACKAGE_VERSION="2.7.0")
target_compile_options(airscreen PRIVATE -O2 -Wno-unused-parameter)

target_link_libraries(airscreen crypto_static mediandk aaudio android log)
