/**
******************************************************************************
* @file egse_tool.cpp
* @brief EGSE 측정 시나리오를 실행하는 명령행 도구입니다.
*
* @details
*
*
******************************************************************************
*/
//
// ┌──────────────────────────────────────────────────────────────┐
// │ EGSE 실측 도구                                                │
// │                                                              │
// │   카메라를 실제로 찍어보면서 AE에 필요한 값을 재는 프로그램.     │
// │   AE_ENABLE_EGSE_TOOLS 를 켜야 빌드된다.                       │
// │                                                              │
// │   빌드:  cmake -DAE_ENABLE_EGSE_TOOLS=ON ..                   │
// │   실행:  ./egse_tool A      (항목 A만)                        │
// │          ./egse_tool all    (전부)                            │
// └──────────────────────────────────────────────────────────────┘
//
// 실제로 쓰려면 아래 SimulatedCamera 자리에 SpaceWire 통신 코드를 넣으면 된다.

#include <cstdio>
#include <cstring>

#ifndef AE_ENABLE_EGSE_TOOLS
int main() {
  std::printf("EGSE 도구가 꺼져 있습니다.\n");
  std::printf("빌드할 때 -DAE_ENABLE_EGSE_TOOLS=ON 을 켜세요.\n");
  return 1;
}
#else

#include "mcamv4_ae/ae_egse_measure.hpp"

using namespace ae;
using namespace ae::egse;

// ==============================================================
// [교체 대상] 실제 카메라 대신 쓰는 흉내 카메라
//
//   실제 구현이 해야 할 일:
//     1) setup 대로 TC(222,1) Take Image 발행
//     2) setup 대로 TC(222,4) Download Image 발행
//     3) 받은 RAW로 존 통계 계산 (ae_zone_stats.cu 커널)
//     4) TM(3,25) HK Report에서 SENSOR_TEMP 읽기
//     5) IMAGE_HEADER의 EXPOSURE1/2를 그대로 채워 넣기
//
//   아래는 도구가 제대로 도는지 확인하기 위한 가짜 구현이다.
//   실제 센서처럼 "밝기 = 조도 x 실제노출시간" 으로 값을 만든다.
// ==============================================================
class SimulatedCamera : public IEgseCamera {
 public:
  // scene_milli: 조도. 실제노출 1us 당 밝기가 몇 x 0.001 만큼 오르는지
  explicit SimulatedCamera(uint32_t scene_milli) : scene_(scene_milli) {}

  ShotResult shoot(const ShotSetup& setup) override {
    ShotResult out;

    // HDR과 12bpp를 함께 못 쓰는 카메라를 흉내내고 싶으면 아래를 켠다
    // if (setup.hdr_enabled && setup.data_format == DataFormat::kRaw12bpp) {
    //   out.success = false; out.fid = 0xDE0A; return out;
    // }

    out.success = true;
    out.sensor_temp_c_q8 = 25 * 256;
    out.header_exposure1 = setup.exposure1;
    out.header_exposure2 = setup.hdr_enabled ? setup.exposure2 : 0u;

    // 실제 노출 시간 = 레지스터 x STEP + 33us
    const uint32_t step_us = (setup.step == ExposureStep::kStep10us) ? 10u : 1000u;
    const uint32_t long_us  = setup.exposure1 * step_us + 33u;
    const uint32_t short_us = setup.hdr_enabled
                                  ? (setup.exposure2 * step_us + 33u) : long_us;

    // 찍는 데 걸리는 시간: 리셋 20ms + 읽기 63ms + 노출
    out.elapsed_ms = (setup.sensor_rst_cfg ? 20u : 0u) + 63u + long_us / 1000u;

    const uint32_t pixel_max = format_pixel_max(setup.data_format);
    const uint32_t pixels_per_zone = 170u * 170u / 2u;

    for (int i = 0; i < ZONE_COUNT_S; ++i) {
      ZoneStatsHost& z = out.zones[i];
      const uint32_t long_value  = brightness_of(long_us, pixel_max);
      const uint32_t short_value = brightness_of(short_us, pixel_max);

      z.cnt_long  = pixels_per_zone;
      z.cnt_short = pixels_per_zone;
      z.acc_long  = static_cast<uint64_t>(long_value)  * pixels_per_zone;
      z.acc_short = static_cast<uint64_t>(short_value) * pixels_per_zone;

      const uint32_t sat_limit  = (pixel_max * 98u) / 100u;
      const uint32_t dark_limit = (pixel_max *  1u) / 100u;
      z.cnt_long_saturated  = (long_value  >= sat_limit)  ? pixels_per_zone : 0u;
      z.cnt_long_dark       = (long_value  <= dark_limit) ? pixels_per_zone : 0u;
      z.cnt_short_saturated = (short_value >= sat_limit)  ? pixels_per_zone : 0u;
      z.cnt_short_dark      = (short_value <= dark_limit) ? pixels_per_zone : 0u;
    }
    return out;
  }

 private:
  // 밝기 = 조도 x 실제노출시간 + 블랙레벨.  포화되면 최대값에서 멈춘다.
  uint32_t brightness_of(uint32_t effective_us, uint32_t pixel_max) const {
    uint64_t value = (static_cast<uint64_t>(effective_us) * scene_) / 1000u;
    value += 8u;                                  // 블랙레벨
    // 12bit 기준으로 만든 뒤 요청한 비트수로 줄인다
    if (pixel_max < 4095u) value = value * pixel_max / 4095u;
    if (value > pixel_max) value = pixel_max;
    return static_cast<uint32_t>(value);
  }

  uint32_t scene_;
};

// ==============================================================
static void print_usage() {
  std::printf("사용법: egse_tool <항목>\n\n");
  std::printf("  A    HDR + 다운로드 포맷 호환 확인   (5분)  ★제일 먼저\n");
  std::printf("  B    선형성 + 33us 검증              (30분)\n");
  std::printf("  C    블랙레벨 잔차 (렌즈캡 필요)     (반나절)\n");
  std::printf("  D    목표 밝기 확정                  (1~2시간)\n");
  std::printf("  E    짧은 노출 목표 확정             (1시간)\n");
  std::printf("  F    노출 반영 지연 확인             (15분)\n");
  std::printf("  all  전부 실행\n\n");
  std::printf("결과는 egse_result.csv 로 저장됩니다.\n");
  std::printf("분석:  python3 egse_analyze.py egse_result.csv\n\n");
  std::printf("※ 이 도구는 '카메라를 직접 제어'하는 경로입니다.\n");
  std::printf("   이미 찍어둔 RAW 파일로 측정하려면 python/ 쪽을 쓰세요:\n");
  std::printf("     python3 egse_raw_tool.py plan B\n");
  std::printf("     python3 egse_raw_tool.py run B shot_plan.csv raw/\n");
}

int main(int argc, char** argv) {
  if (argc < 2) { print_usage(); return 1; }

  const char* item = argv[1];

  // 실제로는 여기에 SpaceWire 카메라 구현을 넣는다
  SimulatedCamera camera(500u);   // 흉내용 조도값
  Measurement measure(camera);

  std::printf("========================================\n");
  std::printf("  MCAMv4 AE 실측 도구 (카메라 직접 제어)\n");
  std::printf("========================================\n");
  std::printf("※ 지금은 흉내 카메라로 돌고 있습니다.\n");
  std::printf("   실제 측정하려면 SimulatedCamera 자리에\n");
  std::printf("   SpaceWire 통신 구현을 넣으세요.\n");

  const bool all = (std::strcmp(item, "all") == 0);

  if (all || std::strcmp(item, "A") == 0) measure.run_A_format_compatibility();
  if (all || std::strcmp(item, "B") == 0) measure.run_B_linearity();
  if (all || std::strcmp(item, "F") == 0) measure.run_F_apply_delay();
  if (all || std::strcmp(item, "C") == 0) measure.run_C_black_level();
  if (all || std::strcmp(item, "D") == 0) measure.run_D_target_brightness();
  if (all || std::strcmp(item, "E") == 0) measure.run_E_short_target();

  if (measure.rows().empty()) { print_usage(); return 1; }

  std::FILE* csv = std::fopen("egse_result.csv", "w");
  if (csv) {
    measure.write_csv(csv);
    std::fclose(csv);
    std::printf("\n총 %zu회 촬영. egse_result.csv 저장 완료.\n",
                measure.rows().size());
  }
  return 0;
}

#endif  // AE_ENABLE_EGSE_TOOLS
