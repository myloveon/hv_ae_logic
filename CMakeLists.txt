cmake_minimum_required(VERSION 3.16)
project(mcamv4_ae LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# AE 모듈은 헤더 온리(header-only), 고정소수점(정수) 전용이다.
# CUDA 커널(ae_zone_stats.cu)만 별도 컴파일이 필요하다.
# ============================================================
add_library(mcamv4_ae INTERFACE)
target_include_directories(mcamv4_ae INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(example_usage example_usage.cpp)
target_link_libraries(example_usage PRIVATE mcamv4_ae)
target_compile_options(example_usage PRIVATE -Wall -Wextra)

# 핵심 수식과 단위 변환을 확인하는 자체 테스트.
add_executable(ae_self_test self_test.cpp)
target_link_libraries(ae_self_test PRIVATE mcamv4_ae)
target_compile_options(ae_self_test PRIVATE -Wall -Wextra)
add_test(NAME ae_self_test COMMAND ae_self_test)

# ============================================================
# EGSE 실측 도구 (지상시험용)
#
#   기본은 꺼져 있다. 비행 소프트웨어에는 들어가지 않는다.
#   켜려면:  cmake -DAE_ENABLE_EGSE_TOOLS=ON ..
#
#   켜면 egse_tool 실행파일이 만들어진다.
#     ./egse_tool A      항목 A만 측정
#     ./egse_tool all    전부 측정
#     python3 egse_analyze.py egse_result.csv   결과 자동 분석
# ============================================================
option(AE_ENABLE_EGSE_TOOLS "EGSE 실측 도구를 빌드한다 (지상시험용)" OFF)

if(AE_ENABLE_EGSE_TOOLS)
    add_executable(egse_tool egse_tool.cpp)
    target_link_libraries(egse_tool PRIVATE mcamv4_ae)
    target_compile_definitions(egse_tool PRIVATE AE_ENABLE_EGSE_TOOLS)
    target_compile_options(egse_tool PRIVATE -Wall -Wextra)
    message(STATUS "EGSE 실측 도구: 켜짐 (egse_tool 이 만들어집니다)")
else()
    message(STATUS "EGSE 실측 도구: 꺼짐 (-DAE_ENABLE_EGSE_TOOLS=ON 으로 켜세요)")
endif()

# ------------------------------------------------------------
# 코딩 규칙 검사 (float/double/exp()/pow() 사용 금지)
#   make check_rules
# ------------------------------------------------------------
add_custom_target(check_rules
    # ZIP 해제 후 실행 권한이 없어도 동작하도록 bash로 호출한다.
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/check_rules.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "정수 전용 코딩 규칙 검사")

# ------------------------------------------------------------
# 존 통계 CUDA 커널 (실제 파이프라인 통합 시 활성화)
#   find_package(CUDAToolkit REQUIRED)
#   enable_language(CUDA)
#   add_library(ae_zone_stats STATIC mcamv4_ae/ae_zone_stats.cu)
#   set_target_properties(ae_zone_stats PROPERTIES CUDA_ARCHITECTURES "87")
#   target_link_libraries(ae_zone_stats PUBLIC mcamv4_ae)
# ------------------------------------------------------------
