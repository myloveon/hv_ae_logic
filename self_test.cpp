// self_test.cpp
//
// 초보자도 핵심 수식의 입력과 기대 결과를 바로 확인할 수 있는 자체 테스트.
// 외부 테스트 프레임워크를 사용하지 않고 표준 C++만 사용한다.

#include <cstdint>
#include <cstdio>

#include "mcamv4_ae/ae_hdr_ratio_schedule.hpp"
#include "mcamv4_ae/ae_log_exposure_axis.hpp"
#include "mcamv4_ae/ae_manager_v2.hpp"
#include "mcamv4_ae/ae_shutter_table_data.hpp"
#include "mcamv4_ae/ae_solver.hpp"
#include "mcamv4_ae/ae_warm_start_cache.hpp"

namespace {

int g_failure_count = 0;

// 역할: uint32_t 실제값과 기대값을 비교하고 실패 수를 누적한다.
void expect_equal_u32(const char* test_name,
                      uint32_t actual,
                      uint32_t expected)
{
  if (actual == expected)
  {
    std::printf("[통과] %s\n", test_name);
    return;
  }

  std::printf("[실패] %s: 실제=%u, 기대=%u\n",
              test_name,
              actual,
              expected);
  ++g_failure_count;
}

// 역할: bool 조건을 검사하고 실패 수를 누적한다.
void expect_true(const char* test_name, bool condition)
{
  if (condition)
  {
    std::printf("[통과] %s\n", test_name);
    return;
  }
  std::printf("[실패] %s\n", test_name);
  ++g_failure_count;
}

// 역할: 촬영 실패 종료 경로를 검증하기 위한 가상 카메라다.
class AlwaysFailCamera : public ae::ICameraCaptureInterface
{
 public:
  ae::CaptureResult capture(const ae::ExposurePair&) override
  {
    ++tick_;
    return {};
  }
  uint64_t now_ms() override { return tick_ * 200u; }
 private:
  uint64_t tick_ = 1u;
};

}  // namespace

// 역할: 핵심 수식, 단위 변환, 오류 처리 회귀 테스트를 실행한다.
int main()
{
  using namespace ae;

  ShutterLUT shutter_lut(kDefaultShutterTable, kDefaultShutterTableCount);

  LogExposureAxisParams axis_params;
  axis_params.code_min			 = 108u;
  axis_params.code_max			 = 994u;
  axis_params.max_nominal_time   = 104000u * fx::kOne_q8;
  axis_params.step = ExposureStep::kStep10us;
  LogExposureAxis axis(shutter_lut, axis_params);

  // 실제 적분시간 = register x 10us + 33us
  expect_equal_u32("+33us 실효 노출시간",
                   axis.register_to_effective_time(53u),
                   563u * fx::kOne_q8);

  AEAlgoParams solver_params;
  solver_params.max_step_ratio = fx::q16_ratio(4u, 1u);
  ExposureSolver solver(solver_params);

  // 563us x 2240 / 1120 = 1126us
  expect_equal_u32("첫 Shot Target/Current 계산",
                   solver.first_shot_inversion(
                       563u * fx::kOne_q8,
                       1120u * fx::kOne_q8,
                       2240u * fx::kOne_q8),
	                   1126u * fx::kOne_q8);

  // 563 + (2240-1120) x (1123-563) / (2100-1120) = 1203us
  expect_equal_u32("두 번째 Shot Secant 계산",
                   solver.secant_step(
                       563u * fx::kOne_q8,
                       1120u * fx::kOne_q8,
                       1123u * fx::kOne_q8,
                       2100u * fx::kOne_q8,
                       2240u * fx::kOne_q8),
	                   1203u * fx::kOne_q8);

  // Long 563us, ratio 8이면 Short 목표는 약 70us이고 가장 가까운 register는 4다.
  const ExposureUs short_target = (563u * fx::kOne_q8) / 8u;
  expect_equal_u32("HDR Short register 계산",
                   axis.effective_time_to_register(short_target),
                   4u);

  // Threshold 모드를 선택했어도 단위가 맞는 표가 없으면 AR0233 line 표를 쓰지 않는다.
  HdrRatioParams fallback_params;
  fallback_params.mode = RatioMode::kThresholdTable;
  fallback_params.short_target_us_q8 = 2000u * fx::kOne_q8;
  HdrRatioSchedule fallback_schedule(fallback_params);
  expect_equal_u32("AR0233 line/us 단위 오사용 방지",
                   fallback_schedule.compute_ratio(16000u * fx::kOne_q8),
                   8u);

  // 단위가 Q8 us인 사용자 임계 테이블은 Threshold 모드에서 그대로 사용할 수 있다.
  const HdrRatioEntry effective_time_table[] =
  {
      {5000u * fx::kOne_q8, 4u},
      {10000u * fx::kOne_q8, 8u},
      {20000u * fx::kOne_q8, 12u},
  };

  HdrRatioSchedule table_schedule(fallback_params);
  table_schedule.set_schedule(effective_time_table, 3u);
  expect_equal_u32("MCAMv4 실효시간 임계 테이블",
                   table_schedule.compute_ratio(9000u * fx::kOne_q8),
                   8u);

  // code -> register -> effective time -> code 왕복 뒤에도 같은 register여야 한다.
  const uint32_t source_code = 400u;
  const uint32_t round_trip_code =
      axis.effective_time_to_code(axis.code_to_effective_time(source_code));
  expect_equal_u32("Code/Register 왕복",
                   axis.code_to_register(round_trip_code),
                   axis.code_to_register(source_code));

  expect_equal_u32("Fallback 목표 일치 시 유지",
                   solver.fallback_large_step(563u * fx::kOne_q8,
                                              2240u * fx::kOne_q8,
                                              2240u * fx::kOne_q8),
							                   563u * fx::kOne_q8);

  std::array<ZoneStatsHost, ZONE_COUNT_S> empty_zones{};
  std::array<uint16_t, ZONE_COUNT_S> weights{};
  weights.fill(100u);
  const MeteringResult empty_metering = compute_weighted_metering(empty_zones, weights);
  expect_true("빈 Zone 유효 개수 제외",
              empty_metering.valid_zone_count == 0 &&
              empty_metering.weighted_mean == 0u &&
              !empty_metering.trust_linear_model);

  WarmStartCache cache;
  expect_true("WarmStart null 출력 포인터 방어", !cache.get_if_valid(0u, nullptr));

  AlwaysFailCamera fail_camera;
  AEManagerV2 fail_manager(fail_camera, axis);
  AESessionConfig fail_config;
  const AESessionResult fail_result = fail_manager.run(fail_config);
  expect_true("촬영 실패 시 마지막 Exposure 보존",
              fail_result.exit_reason == AEExitReason::kCaptureFailed &&
              fail_result.final_exposure.exposure1 >= 1u &&
              fail_result.final_exposure.exposure2 >= 1u);

  if (g_failure_count != 0)
  {
    std::printf("\n총 실패: %d\n", g_failure_count);
    return 1;
  }

  std::printf("\n모든 자체 테스트가 통과했습니다.\n");
  return 0;
}
