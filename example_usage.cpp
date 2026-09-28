/**
******************************************************************************
* @file example_usage.cpp
* @brief 모의 카메라를 이용한 MCAMv4 AE 모듈의 기본 사용 예제를 제공합니다.
*
* @details
* 
* 
******************************************************************************
*/
// MCAMv4 AE 모듈 사용 예제 (고정소수점 전용).
// 예제와 알고리즘 모두 float/double을 사용하지 않는다.

#include <cstdio>
#include <cstdint>
#include "mcamv4_ae/ae_manager_v2.hpp"
#include "mcamv4_ae/ae_shutter_table_data.hpp"

using namespace ae;

// ============================================================
// [교체 대상] 실제 카메라 인터페이스 구현
// 아래는 알고리즘 검증용 가상 카메라 (정수 LCG 노이즈 사용)
// ============================================================
class MockCamera : public ICameraCaptureInterface
{
 public:
  // 역할: 선형 장면 gain과 정수 노이즈를 사용하는 가상 카메라를 만든다.
  MockCamera(Ratio scene_gain, uint32_t noise)
      : scene_gain_(scene_gain), noise_(noise)
	{
	}

  // 역할: 장면 급변 시험을 위해 Q16 장면 gain을 변경한다.
  void set_scene_gain(Ratio gain) { scene_gain_ = gain; }		//fx::q16_ratio(1, 2) :: 0.5

  // 역할: 요청 Exposure에서 가상 Long/Short Zone 통계를 생성한다.
  CaptureResult capture(const ExposurePair& exposure) override
  {
    CaptureResult result;
    result.success = true;				//<----------
    result.sensor_temp_c_q8 = 25u * fx::kOne_q8;  //25C * 256;

    // 실제 물리: 밝기 = 장면반사율 x 노출시간(us) + 블랙레벨
    const uint32_t time_us = exposure.long_effective_time() >> fx::kQ8; 	//Long

    uint64_t brightness =
        (static_cast<uint64_t>(time_us) * scene_gain_) >> fx::kQ16;

    brightness += 8;						//Black Offset

	if (brightness > PIXEL_MAX_VALUE)		//12bpp
	{
		brightness = PIXEL_MAX_VALUE;
	}

    const uint32_t pixels = 170u * 170u / 2u; 	//pixel Zone Size

    for (int i = 0; i < ZONE_COUNT_S; ++i)	//12x12
	{
      const uint32_t long_value = add_noise(static_cast<uint32_t>(brightness));

      const uint32_t short_value = static_cast<uint32_t>(
          (static_cast<uint64_t>(long_value) * exposure.short_effective_time()) /
          exposure.long_effective_time());

      ZoneStatsHost& zone = result.zones[i];
      zone.cnt_long		 = pixels;			//14,450
      zone.cnt_short	 = pixels;			//14,450
      zone.acc_long		 = static_cast<uint64_t>(long_value) * pixels;
      zone.acc_short	 = static_cast<uint64_t>(short_value) * pixels;

      zone.cnt_long_saturated	= (long_value >= 4000u) 	? pixels : 0u;
      zone.cnt_long_dark 		= (long_value <= 32u) 		? pixels : 0u;

      zone.cnt_short_saturated 	= (short_value >= 4000u) 	? pixels : 0u;
      zone.cnt_short_dark 		= (short_value <= 32u) 		? pixels : 0u;
    }

    ++tick_;		//Tick Count 1s/5fps = 0.2s = 200ms
    return result;
  }

  // 역할: 200ms 프레임 간격을 가정한 단조 증가 시각을 반환한다.
  uint64_t now_ms() override { return tick_ * 200ull; }

 private:
  // 역할: 결정적 LCG 노이즈를 더하고 결과를 12bit로 제한한다.
  uint32_t add_noise(uint32_t value)
  {
    if (noise_ == 0)
		return value;

    seed_ = seed_ * 1103515245u + 12345u;	//단순 정수 의사난수

    const int32_t noise =
        static_cast<int32_t>((seed_ >> 16) % (2u * noise_ + 1u)) -	// seed_ % (2x50+1) = 0 ~ 100
        static_cast<int32_t>(noise_);								// (0 ~ 100) -50 =-50 ~ 50

    int32_t result = static_cast<int32_t>(value) + noise;

    if (result < 0) result = 0;

    if (result > PIXEL_MAX_VALUE) //12bps:0~4095
		result = PIXEL_MAX_VALUE;

    return static_cast<uint32_t>(result);
  }

  Ratio scene_gain_;
  uint32_t noise_;
  uint32_t seed_ = 12345u;
  uint64_t tick_ = 1;
};

// 역할: 종료 열거값을 출력용 한글 문자열로 변환한다.
static const char* reason_str(AEExitReason reason) {
  switch (reason) {
    case AEExitReason::kConvergedInDeadband: return "데드밴드수렴";
    case AEExitReason::kConvergedNoChange:   return "코드불변수렴";
    case AEExitReason::kOutOfRange:          return "범위밖(불가)";
    case AEExitReason::kBudgetExhausted:     return "예산소진";
    case AEExitReason::kCaptureFailed:       return "촬영실패";
  }
  return "?";
}

// 역할: Scene 열거값을 출력용 문자열로 변환한다.
static const char* scene_str(SceneType scene) {
  switch (scene) {
    case SceneType::kNormal:       return "Normal";
    case SceneType::kBrightTop:    return "BrightTop";
    case SceneType::kDarkTop:      return "DarkTop";
    case SceneType::kHighContrast: return "HighContrast";
  }
  return "?";
}

// 역할: Mock Camera AE 세션과 격자 진단 예제를 실행한다.
int main()
{
  // [1] 셔터 LUT + 로그 노출 축
  ShutterLUT lut(kDefaultShutterTable, kDefaultShutterTableCount);

  LogExposureAxisParams axis_params;
  axis_params.code_min = 108;
  axis_params.code_max = 994;
  axis_params.max_nominal_time = 104000u * fx::kOne_q8;		// 5fps 실효 상한
  axis_params.step = ExposureStep::kStep10us;
  LogExposureAxis axis(lut, axis_params);

  // [2] 카메라 (실제로는 SpaceWire 드라이버)
  MockCamera camera(fx::q16_ratio(1, 2), 50);	//-Gian -Noise
  // [3] AE 매니저
// 기본값: 5샷, tol 0.04, step 2.0
// 격자여유 0.6, 방향성 0.60/0.90
  AEManagerV2 manager(camera, axis);

  HdrRatioParams ratio_params;
  ratio_params.short_target_us_q8 = 2000u * fx::kOne_q8;	// TODO: EGSE 실측 확정
  manager.set_ratio_schedule(ratio_params);

  // [4] 세션 실행
  AESessionConfig config;
  config.target_brightness = 2240u * fx::kOne_q8;			// TODO: EGSE 실측 확정

  std::printf("=== MCAMv4 AE readable fixed-point ===\n");
  std::printf("effective range %u ~ %u us, target %u\n\n",
              axis.code_to_effective_time(axis.code_min()) >> fx::kQ8,
              axis.code_to_effective_time(axis.code_max()) >> fx::kQ8,
              config.target_brightness >> fx::kQ8);

  std::printf("%-5s %3s %-13s %-10s %5s %4s %7s %6s %8s\n",
              "세션", "샷", "종료사유", "씬", "ratio", "전환", "E1", "E2", "밝기");

  for (int session = 1; session <= 6; ++session)
  {
    if (session == 4)
		camera.set_scene_gain(fx::q16_ratio(2, 1));	// 장면 급변 x2

    const AESessionResult result = manager.run(config);

    std::printf("%-5d %3d %-13s %-10s %5u %4d %7u %6u %8u%s\n",
                session,
                result.shots_used,
                reason_str(result.exit_reason),
                scene_str(result.scene),
                result.hdr_ratio,
                result.reversals,
                result.final_exposure.exposure1,
                result.final_exposure.exposure2,
                result.final_brightness >> fx::kQ8,
                result.hdr_collapsed ? "  <- HDR 무력(long=short)"
                                     : ((session == 4) ? "  <- 장면급변" : ""));
  }

  // [5] 진단: 하드웨어 해상도 한계 구간 확인
  QuantizationGuard guard(axis);

  std::printf("\n%8s %12s %8s %10s %8s\n",
              "code", "effective_us", "EXPn", "deadband", "grid");

  for (uint32_t code : {108u, 200u, 300u, 400u, 500u, 700u, 900u})
  {
    const Brightness deadband = guard.dynamic_deadband(code, config.target_brightness);
    std::printf("%8u %12u %8u %9u%% %8s\n",
                code,
                axis.code_to_effective_time(code) >> fx::kQ8,
                axis.code_to_register(code),
                (deadband * 100u) / config.target_brightness,
                guard.is_grid_limited(code, config.target_brightness) ? "YES" : "-");
  }

  return 0;
}
