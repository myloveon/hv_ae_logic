# 함수별 주석 및 호출 관계

소스의 각 함수 앞에는 역할 중심의 한글 주석을 두었습니다. 이 문서는 함수를
파일별로 빠르게 찾기 위한 색인입니다.

| 파일 | 함수 | 역할 |
|---|---|---|
| `ae_manager_v2.hpp` | `run()` | 한 AE 세션의 촬영·측광·노출계산·수렴판정 실행 |
|  | `choose_session_ratio()` | 수동/웜스타트/콜드스타트 Ratio 선택 |
|  | `estimate_next_exposure()` | 신뢰도와 이력에 따라 Inversion/Secant/Fallback 선택 |
|  | `geometric_center_code()` | 콜드스타트 로그축 중앙 계산 |
|  | `at_axis_limit()` | 셔터 제어 범위 끝 판정 |
|  | `build_exposure()` | Long 코드에서 Long/Short Register 생성 |
|  | `finalize()` | 정상 종료 결과·Damping·웜캐시 갱신 |
|  | `finish_failed_session()` | 실패 결과에 마지막 Exposure 보존 |
| `ae_solver.hpp` | `first_shot_inversion()` | 첫 측정 Target/Current 역산 |
|  | `secant_step()` | 현재 세션 두 점의 Secant 계산 |
|  | `fallback_large_step()` | 기울기를 못 믿을 때 방향성 이동 |
|  | `clamp_step()` | 1회 최대 변화 배율 제한 |
| `ae_log_exposure_axis.hpp` (공개) | `code_to_effective_time()` | 눈금→실제시간 (배열 조회) |
|  | `effective_time_to_code()` | 실제시간→눈금 (이분탐색) |
|  | `code_to_register()` | 눈금→센서 Register (배열 조회) |
|  | `register_to_effective_time()` | Register→실제시간 |
|  | `effective_time_to_register()` | 실제시간→Register |
|  | `is_same_register()` | 두 눈금이 같은 Register인지 |
|  | `nominal_step_ratio()` | 눈금 1칸의 밝기 변화율 (배열 조회) |
| `ae_log_exposure_axis.hpp` (private, 표 만들기) | `build_tables()` | 시작 시 887칸 표를 한 번에 생성 |
|  | `weight_to_command_time()` | LUT 가중치→명령시간 (생성 시에만) |
|  | `command_time_to_register()` | 명령시간→Register (생성 시에만) |
|  | `find_closest_index()` | 목표 시간에 가장 가까운 표 위치 |
| `ae_quantization_guard.hpp` | `dynamic_deadband()` | 로그축과 Register 격자를 반영한 허용오차 |
|  | `register_grid_deadband()` | 실제 한 Register 변화 기반 하한 |
|  | `is_converged()` | 밝기/코드 변화 기준 수렴 판정 |
|  | `resolve_code_with_hysteresis()` | 방향성 히스테리시스 적용 |
| `ae_scene_classifier.hpp` | `classify()` | 첫 프레임 Scene과 고정 가중치 결정 |
|  | `build_weights()` | Scene별 144개 상대 가중치 생성 |
|  | `compute_weighted_metering()` | Long 유효 Zone 가중 평균 계산 |
| `ae_hdr_ratio_schedule.hpp` | `compute_ratio()` | Long 실효시간에서 Ratio 후보 계산 |
|  | `decide_session_ratio()` | 세션간 Ratio 히스테리시스 적용 |
|  | `derive_from_short_target()` | Pinned Short 방식 Ratio 양자화 |
| `ae_adaptive_damping.hpp` | `observe()` | 노출 이동 방향 반전 횟수 기록 |
|  | `end_session()` | 정상 세션 결과로 변화 배율 학습 |
|  | `cancel_session()` | 실패 세션 학습 취소 |
| `ae_warm_start_cache.hpp` | `update()` | 수렴 결과 저장 |
|  | `get_if_valid()` | 유효시간과 포인터 검사 후 캐시 반환 |
| `ae_black_level.hpp` | `residual_q8()` | 온도/노출시간 기반 잔차 계산 |
| `ae_shutter_lut.hpp` | `code_to_q7()` / `q7_to_code()` | Shutter Table 양방향 보간 |
| `ae_fixed.hpp` | `scale_q16()` / `unscale_q16()` | Q16 비율 곱셈/나눗셈 |
|  | `isqrt64()` / `geometric_mean()` | 실수형 없는 정수 기하평균 |
|  | `closer_in_log()` | `log()` 없는 Ratio 후보 비교 |
| `ae_zone_stats.cu` | `ae_zone_stats_kernel()` | 12x12 Long/Short Zone 통계 계산 |
|  | `launch_ae_zone_stats()` | 고정 CUDA 실행 구성으로 커널 실행 |

`example_usage.cpp`의 `MockCamera`는 실제 카메라 구현 위치를 보여주며,
`self_test.cpp`의 함수들은 수식과 오류 처리의 회귀 검증을 담당합니다.
