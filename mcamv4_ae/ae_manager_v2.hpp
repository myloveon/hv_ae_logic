// ae_manager_v2.hpp
//
// AE 최상위 흐름. 이 파일에서는 Q-format 연산 자체보다
// "무엇을 판단하는가"가 먼저 읽히도록 구성한다.

// AE 전체를 지휘하는 최상위 클래스 (정수 전용).
//
// ── 설계 원칙 ──────────────────────────────────────────────
// AEManager는 "지휘자"일 뿐, 계산은 전문 부품에게 맡긴다.
//   밝기 측정 -> SceneClassifier + compute_weighted_metering()
//   노출 계산 -> ExposureSolver (secant)
//   축 변환   -> LogExposureAxis (로그 <-> us <-> 레지스터)
//   헌팅 방지 -> QuantizationGuard (격자 데드밴드 + 방향성 히스테리시스)
//   감쇠 조절 -> AdaptiveDamping (부호반전 기반)
//   초기값    -> WarmStartCache
//   HDR 비율  -> HdrRatioSchedule
//
// ── 5샷 예산 안에 끝내기 위한 장치 ─────────────────────────
//   1) 웜스타트 : 직전 성공값에서 시작 -> 보통 1~2샷
//   2) secant   : 실측 2점으로 기울기를 직접 계산 -> 초선형 수렴
//   3) 조기종료 : 레지스터가 안 바뀌면 촬영 생략


#pragma once
#include <cstdint>
#include "ae_types.hpp"
#include "ae_camera_interface.hpp"
#include "ae_scene_classifier.hpp"
#include "ae_solver.hpp"
#include "ae_black_level.hpp"
#include "ae_warm_start_cache.hpp"
#include "ae_log_exposure_axis.hpp"
#include "ae_quantization_guard.hpp"
#include "ae_hdr_ratio_schedule.hpp"
#include "ae_adaptive_damping.hpp"

namespace ae {

enum class AEExitReason
{
  kConvergedInDeadband,   // 밝기가 목표 밝기 안에 들어옴 (정상)
  kConvergedNoChange,     // 더 조정해도 레지스터가 안 바뀜 (도달 가능한 최선)
  kOutOfRange,            // 노출 눈금자 한계에 부딪혀 목표에 도달 불가.
                          //   셔터만으로는 방법이 없는 구간이므로,
                          //   상위 SW는 게인 조절 등 다른 수단을 써야 한다.
  kBudgetExhausted,       // 촬영 예산을 다 씀	(fail-safe)
  kCaptureFailed,         // 촬영이 계속 실패
};

struct AESessionResult
{
	//정상적으로 사용할 수 있는 노출로 종료했는지
	bool converged = false;
	//수렴, 범위 초과, 촬영 실패 등의 종료 이유
	AEExitReason exit_reason = AEExitReason::kBudgetExhausted;
	//촬영을 시도한 횟수
	int shots_used = 0;
	//최종 로그 노출 code
	uint32_t final_code = 0;
	//최종 Long/Short 센서 Register
	ExposurePair final_exposure;
	//마지막 또는 최선의 Long 밝기
	Brightness final_brightness = 0;

	SceneType scene = SceneType::kNormal;
	// 이번 세션에 실제 사용한 비율
	uint32_t hdr_ratio		 = 0;
	// 수렴 노출로 재계산한 Ratio 값(다음 세션용)s
	uint32_t next_hdr_ratio	 = 0;
	// 긴 노출과 짧은 노출이 같아져서 HDR이 사실상 꺼진 상태인가?
	//
	//   아주 밝은 장면에서는 long 자체가 이미 최소 노출(43us)이라
	//   short를 더 줄일 방법이 없다. 그러면 ratio가 1이 되고
	//   두 노출이 같아져서 HDR로 얻을 게 없어진다.
	//
	//   알고리즘 잘못이 아니라 물리적 한계지만, 상위 SW는 이 사실을
	//   알아야 한다. HDR 합성을 건너뛰거나 게인을 줄이는 판단이 필요하다.
	bool hdr_collapsed		 = false;
	// 이동 방향 전환 횟수 (헌팅 지표)
	int reversals				 = 0;
	//이번 세션의 최대 노출 변화 배윯
	Ratio step_ratio_used		 = 0;
};

struct AESessionConfig
{
  // 목표 밝기 (Q8). Merge 이전 raw long 채널, 12bit 도메인.
  Brightness target_brightness	 = 2240u * fx::kOne_q8;		//2240 x 256 = 357,440 <==
  // 0 = HdrRatioSchedule이 자동 결정(권장). 0이 아니면 그 값으로 고정.
  uint32_t hdr_ratio			 = 0;
  // 0 = 로그 축 기하중앙 자동 계산.
  // 콜드스타트 시작점은 장면과 무관하므로 양방향 대칭인 중앙이 최선이다.
  uint32_t cold_start_code 		 = 0;
  // 콜드스타트 기본 ratio. 시작 노출로 ratio를 정하면 의미가 없으므로
  // 보수적 기본값으로 진행하고 수렴 후 재계산한다.
  uint32_t cold_start_ratio		 = 8;
  // 콜드스타트 rate limit (Q16). 도달 자체가 우선이므로 완화한다.
  // 5샷 = 4회 조정, 4배 제한이면 4^4 = 256배(8스톱)까지 이동 가능하여
  // 기하중앙에서 양끝(각 7.2스톱)을 모두 커버한다.
  Ratio cold_start_max_step_ratio = fx::q16_ratio(4, 1);
};

class AEManagerV2
{
 public:
  // 역할: 카메라 I/O와 AE 계산 모듈을 연결한다.
  // 주의: camera와 axis는 이 객체보다 오래 살아 있어야 한다.
  AEManagerV2(ICameraCaptureInterface& camera,
              const LogExposureAxis& axis,
              AEAlgoParams algo = {},
              SceneClassifierParams scene = {},
              QuantizationGuardParams guard = {})
      : camera_(camera),
        axis_(axis),
        classifier_(scene),
        guard_(axis, guard),
        algo_(algo) {}

  // 역할: 선택적 블랙레벨 잔차 보정 모델을 등록한다.
  void set_black_level(const BlackLevelModel& model)
  {
  	black_level_ = model;
  }
  // 역할: HDR Ratio 결정 방식과 파라미터를 설정한다.
  void set_ratio_schedule(HdrRatioParams params)
  {
    ratio_schedule_ = HdrRatioSchedule(params);
  }

  // ------------------------------------------------------------
  // AE 세션 실행
  // ------------------------------------------------------------
  // 역할: Threshold Table 모드의 MCAMv4 실효시간(Q8 us) 표를 등록한다.
  // table 메모리는 AEManagerV2보다 오래 유지되어야 한다.
  void set_ratio_threshold_table(const HdrRatioEntry* table, uint32_t count)
  {
    ratio_schedule_.set_schedule(table, count);
  }

  // 역할: 최대 max_iterations Shot 안에서 한 번의 HDR AE 세션을 실행한다.
  // 반환: 수렴 여부, 종료 이유, 최종 Exposure와 진단 정보.
  AESessionResult run(const AESessionConfig& config)
  {
    AESessionResult result;
    const uint64_t now = camera_.now_ms();

    // ===== [1] 시작 노출 결정 (Warm Start 확인) =====
    WarmStartEntry warm;
    const bool warm_start_available = warm_start_.get_if_valid(now, &warm);
    const bool cold_start = !warm_start_available;

    uint32_t code = 0;

    // Secant는 반드시 "현재 세션에서 실제로 측정한 두 점"만 사용한다.
    // 이전 세션의 밝기는 장면이 바뀌었을 수 있으므로 기울기 계산에 섞지 않는다.
    bool previous_sample_available	 = false;
    ExposureUs previous_time		 = 0;
    Brightness previous_brightness	 = 0;

    if (warm_start_available)
	{
      // 웜스타트에서는 시작 노출 코드만 재사용한다.
      // warm.brightness_q8은 진단용 캐시이며 현재 세션의 Secant 점이 아니다.
      code = warm.code;
    }
	else
	{
      code = (config.cold_start_code != 0)
          ? config.cold_start_code			//cold_start_code가 0이 아니면 사용자가 지정값
          : geometric_center_code();		// 노출축 최소와 최대의 기하평균 위치
    }
    // ===== HDR ratio 결정 (세션당 1회, 이후 고정) =====
    const uint32_t session_ratio = choose_session_ratio(config, warm_start_available, warm);
    result.hdr_ratio = session_ratio;		//x8:x1

    // ===== 세션 상태 초기화 =====
    guard_.reset_direction();

    damping_.begin_session();

    // rate limit: 콜드스타트는 완화, 웜스타트는 적응형 감쇠 학습값
    AEAlgoParams active_algo = algo_;

    active_algo.max_step_ratio = cold_start
        ? config.cold_start_max_step_ratio
        : damping_.step_ratio();

    result.step_ratio_used = active_algo.max_step_ratio;
    ExposureSolver solver(active_algo);

    // 수렴 실패에 대비한 최선값 기록 (헌팅 차단용)
    uint32_t best_code 			 = code;
    Brightness best_brightness	 = 0;
    uint32_t best_error			 = 0xFFFFFFFFu;
    bool best_sample_available	 = false;

    SceneClassification 		scene;			//scene 종류와 144개 Zone 가중치를 저장
    bool scene_locked		 		= false;	//Scene과 Zone 가중치를 고정함
    int consecutive_failures 		= 0;		//연속 촬영 실패 횟수
    TemperatureC last_temperature	= 0;		// 예산 소진 시 캐시에 남길 온도

    // ===== [2~8] 반복 루프 =====
    for (int shot = 1; shot <= algo_.max_iterations; ++shot)
	{
      const ExposurePair exposure = build_exposure(code, session_ratio);
      const CaptureResult capture = camera_.capture(exposure);
      result.shots_used = shot;

	  //Fail Capture
      if (!capture.success)
	  {
        ++consecutive_failures;
        if (consecutive_failures >= 3)
		{//three fail
          finish_failed_session(result, AEExitReason::kCaptureFailed,
                                code, session_ratio);
          return result;
        }
        continue;
      }

      consecutive_failures = 0;
	  //Temperature storage
      last_temperature = capture.sensor_temp_c_q8;

      // --- 씬 판정 (1샷에서만!) ---
      if (!scene_locked)
	  {
        scene = classifier_.classify(capture.zones);
        result.scene = scene.type;
        scene_locked = true;
      }

      // --- 가중 측광 -> 밝기 스칼라 (Q8) ---
      const MeteringResult metering = compute_weighted_metering(
          capture.zones,											//Long/Short Zone 통계 144개
          scene.weights,											//Scene에 따라 고정한 가중치
          fx::percent(50),											//Long 포화 픽셀이 50% 이상인 Zone 제외
          fx::percent(90),											//Long 암부 픽셀이 90% 이상인 Zone 제외
          algo_.min_valid_zones);									//선형 모델을 신뢰할 최소 Zone 수, 기본 30

      const ExposureUs current_time = axis_.code_to_effective_time(code);

	  //Black Level
      const Brightness black_residual = black_level_.residual_q8(
          capture.sensor_temp_c_q8,
          current_time >> fx::kQ8);

      //최종 제어 밝기 계산
      const Brightness current_brightness =
          (metering.weighted_mean > black_residual)
              ? (metering.weighted_mean - black_residual)				//현재밝기=가중평균밝기−BlackLevel
              : 0u;

      result.final_brightness = current_brightness;

      // 최선값 갱신
      const uint32_t error = fx::abs_diff(current_brightness, config.target_brightness);		//<-- error= |Current−Target|

      if (error < best_error)
	  {
        best_error		 = error;
        best_code		 = code;
        best_brightness	 = current_brightness;
        best_sample_available = true;
      }

      // --- 다음 노출 계산 ---
      const ExposureUs ideal_time = estimate_next_exposure(
          solver,
          metering,
          current_time,
          current_brightness,
          previous_sample_available,
          previous_time,
          previous_brightness,
          config.target_brightness);

      // --- 수렴 판정 ---
      if (guard_.is_converged(code,
                              current_brightness,
                              config.target_brightness,			//2240u * fx::kOne_q8
                              ideal_time))
      {
        // 오차가 데드밴드 안이면 진짜 수렴,
        // 아니면 "더 못 바꾸는 상태"인데 이때 두 경우를 구분해야 한다.
        //   - 눈금자 한가운데인데 못 바꿈  -> 레지스터 해상도 한계 (오차 작음)
        //   - 눈금자 끝에 붙어서 못 바꿈    -> 노출 범위 밖 (오차 큼, 화면이 망가짐)
        // 후자를 converged로 보고하면 상위 SW가 잘못된 노출로 촬영을 진행한다.
        if (error <= guard_.dynamic_deadband(code, config.target_brightness))	//target_brightness = 2240u * fx::kOne_q8
		{
          result.converged = true;
          result.exit_reason = AEExitReason::kConvergedInDeadband;
        }
		else if (at_axis_limit(code))
		{
          result.converged = false;
          result.exit_reason = AEExitReason::kOutOfRange;
        }
		else
		{
          result.converged = true;
          result.exit_reason = AEExitReason::kConvergedNoChange;
        }

        finalize(result, code, session_ratio, current_brightness,
                 capture.sensor_temp_c_q8);
        return result;
      }

      // --- 로그 코드 변환 + 방향성 히스테리시스 ---
      const uint32_t next_code =
          guard_.resolve_code_with_hysteresis(code, ideal_time);

      // --- 레지스터가 실제로 바뀌는지 확인 ---
      // 짧은 노출 구간에서는 로그 코드가 달라져도 레지스터 정수격자에서
      // 같은 값으로 뭉개질 수 있다. 촬영해봤자 결과가 같으므로 예산만 낭비된다.
      if (axis_.is_same_register(code, next_code))
	  {
        // 참고: 여기 도달했다는 것은 바로 위 is_converged()가 false였다는 뜻이고,
        //       is_converged()의 첫 조건이 "오차 <= 데드밴드"이므로
        //       이 지점에서 오차는 반드시 데드밴드 "밖"이다.
        //       따라서 남은 판단은 "축 끝인가 아닌가" 하나뿐이다.
        //         - 축 중간인데 레지스터가 안 바뀜 -> 해상도 한계 (정상 종료)
        //         - 축 끝이라 못 움직임             -> 노출 범위 밖 (실패로 보고)
        const bool out_of_range = at_axis_limit(code);
        result.converged = !out_of_range;
        result.exit_reason = out_of_range ? AEExitReason::kOutOfRange
                                          : AEExitReason::kConvergedNoChange;

        finalize(result, code, session_ratio, current_brightness,
                 capture.sensor_temp_c_q8);

		return result;
      }

      damping_.observe((next_code > code) ? +1 : -1);

      previous_time = current_time;
      previous_brightness = current_brightness;
      previous_sample_available = true;
      code = next_code;
    }

    // ===== 예산 소진: fail-safe =====
    result.exit_reason = AEExitReason::kBudgetExhausted;
    result.reversals = damping_.reversals_this_session();
    damping_.end_session();

    if (best_sample_available)
	{
      result.final_code = best_code;
      result.final_brightness = best_brightness;
      result.final_exposure = build_exposure(best_code, session_ratio);
      result.hdr_collapsed =
          (result.final_exposure.exposure1 == result.final_exposure.exposure2);
      // 실패해도 최선값을 캐시에 남긴다.
      // 없으면 "실패 -> 캐시없음 -> 다음 세션도 같은 자리 -> 또 실패"의 악순환에 빠진다.

      result.next_hdr_ratio = ratio_schedule_.decide_session_ratio(
          axis_.code_to_effective_time(best_code), session_ratio);

      warm_start_.update(best_code, best_brightness, last_temperature,
                         result.next_hdr_ratio, camera_.now_ms());
    }
	else
	{
      result.final_code = code;
      result.final_exposure = build_exposure(code, session_ratio);
      result.hdr_collapsed =
          (result.final_exposure.exposure1 == result.final_exposure.exposure2);
    }

    return result;
  }

 private:
  // 역할: 수동값, 웜스타트값, 콜드스타트 기본값 순으로 Ratio를 고른다.
  uint32_t choose_session_ratio(const AESessionConfig& config,
                                bool warm_available,
                                const WarmStartEntry& warm) const
  {
    if (config.hdr_ratio > 0)
		return config.hdr_ratio;

    if (warm_available && warm.hdr_ratio > 0)
		return warm.hdr_ratio;

    return config.cold_start_ratio;		//Long : Short = 8 : 1
  }

  // 역할: 측광 신뢰도와 세션 이력에 따라 다음 노출 계산법을 선택한다.
  ExposureUs estimate_next_exposure(const ExposureSolver& solver,
                                    const MeteringResult& metering,
                                    ExposureUs current_time,
                                    Brightness current_brightness,
                                    bool previous_available,
                                    ExposureUs previous_time,
                                    Brightness previous_brightness,
                                    Brightness target) const
{
    if (!metering.trust_linear_model)	//유효 Zone 30 미만  부족
	{
      return solver.fallback_large_step(current_time, current_brightness, target);
    }

    if (previous_available)				//현재 세션의 이전 측정점
	{
      return solver.secant_step(previous_time, previous_brightness,
                                current_time, current_brightness, target);
    }

	//첫 번째 신뢰 측정
    return solver.first_shot_inversion(current_time, current_brightness, target);
  }

  // 역할: 로그축 양 끝 실효시간의 정수 기하평균에 가까운 코드를 구한다.
  uint32_t geometric_center_code() const
  {
    const ExposureUs low = axis_.code_to_effective_time(axis_.code_min());	//code_min = 108;
    const ExposureUs high = axis_.code_to_effective_time(axis_.code_max());	//code_max = 994;

    return axis_.effective_time_to_code(fx::geometric_mean(low, high));
  }

  // 역할: 현재 코드가 제어축 최소 또는 최대 끝인지 판정한다.
  bool at_axis_limit(uint32_t code) const
  {
    return (code <= axis_.code_min()) || (code >= axis_.code_max());
  }

  // 역할: 로그 Long 코드와 Ratio를 Long/Short Register 쌍으로 변환한다.
  ExposurePair build_exposure(uint32_t code, uint32_t hdr_ratio) const
  {
    ExposurePair exposure;
    exposure.step = axis_.step();
	//Long
    exposure.exposure1 = axis_.code_to_register(code);

    // short: "시간 도메인"에서 ratio를 나눈 뒤 레지스터로.
    // (코드에서 직접 나누면 안 됨 - 로그 인코딩이라 의미가 다르다)
    const ExposureUs long_time = axis_.code_to_effective_time(code);

    const ExposureUs short_time = (hdr_ratio > 0)
        ? (long_time / hdr_ratio)
        : long_time;

	//Short
    exposure.exposure2 = axis_.effective_time_to_register(short_time);

    // HDR 가드: 둘 다 0이면 FID_PICTURE_HDR_DARK 오류
    if (exposure.exposure1 < 1u) exposure.exposure1 = 1u;
    if (exposure.exposure2 < 1u) exposure.exposure2 = 1u;

	return exposure;
  }

  // 역할: 정상 종료 결과와 Damping, 웜스타트 캐시를 갱신한다.
  void finalize(AESessionResult& result,
                uint32_t code,
                uint32_t session_ratio,
                Brightness brightness,
                TemperatureC temperature)
  {
    result.reversals = damping_.reversals_this_session();
    damping_.end_session();

    result.final_code = code;
    result.final_exposure = build_exposure(code, session_ratio);
    result.hdr_collapsed =
        (result.final_exposure.exposure1 == result.final_exposure.exposure2);
    // 수렴된 노출이야말로 장면을 대표하는 값이다.
    // 이 값으로 ratio를 재계산해 캐시에 남기면 다음 세션이 장면에 맞게 시작한다.
    result.next_hdr_ratio = ratio_schedule_.decide_session_ratio(
        axis_.code_to_effective_time(code), session_ratio);

    warm_start_.update(code, brightness, temperature,
                       result.next_hdr_ratio, camera_.now_ms());
  }

  // 역할: 촬영 실패 결과에 마지막 요청 Exposure를 보존한다.
  // 주의: 실패 세션은 Damping과 웜스타트 학습에 사용하지 않는다.
  void finish_failed_session(AESessionResult& result,
                             AEExitReason reason,
                             uint32_t code,
                             uint32_t session_ratio)
  {
    result.converged		= false;
    result.exit_reason		= reason;
    result.reversals		= damping_.reversals_this_session();
    result.final_code		= code;
    result.final_exposure	= build_exposure(code, session_ratio);
    result.next_hdr_ratio	= session_ratio;
    damping_.cancel_session();
  }

  ICameraCaptureInterface& camera_;
  const LogExposureAxis& axis_;			//kDefaultShutterTable
  SceneClassifier classifier_;
  QuantizationGuard guard_;
  AEAlgoParams algo_;
  BlackLevelModel black_level_;
  WarmStartCache warm_start_;
  HdrRatioSchedule ratio_schedule_;
  AdaptiveDamping damping_;
};

}  // namespace ae
