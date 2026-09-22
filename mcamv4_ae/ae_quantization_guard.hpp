// ae_quantization_guard.hpp
//
// 양자화 격자 + 방향성 히스테리시스.
// 고정 비율은 Fraction으로 표현해 알고리즘 본문에서 Q16 숫자를 숨긴다.

// 양자화(계단형) 액추에이터 대응 + 헌팅 방지. 정수 전용.
//
// ── 3원칙 ──────────────────────────────────────────────────
//  1) 데드밴드를 고정값이 아니라 "현재 위치의 1스텝 밝기변화량"에 맞춰 동적 계산.
//     여기에 **레지스터 정수 격자 기반 하한**을 반드시 더한다.
//  2) 수렴 판정을 "코드가 더 이상 바뀌지 않음"으로 한다.
//     도달 불가능한 정밀도를 요구하지 않는 것이 핵심.
//  3) **방향성 히스테리시스** - 진행 방향은 민감하게, 방향 전환은 둔감하게.

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"
#include "ae_log_exposure_axis.hpp"

namespace ae {

struct QuantizationGuardParams {
  // 로그 1스텝 밝기변화량에 곱할 계수 (Q16)
  fx::Fraction deadband_step_fraction = fx::percent(70);	// 0.7
  // 데드밴드 상/하한 (목표밝기 대비 Q16)
  fx::Fraction min_deadband = fx::percent(2);				// 0.02
  fx::Fraction max_deadband = fx::percent(25);				// 0.25
 // ★ 레지스터 정수 격자 기반 데드밴드 하한 계수 (Q16).
  // 반올림 시 최대 "격자 절반(0.5)"만큼 어긋나므로, 도달 보장을 위해
  // 0.5보다 커야 한다. 이 하한은 물리적 한계이므로 max_deadband보다 우선한다.
  fx::Fraction grid_deadband_fraction = fx::percent(60);	// 0.6

 // ★ 방향성 히스테리시스 (Kodak 특허 US6900840/US6970198 방식).
  // 원 특허는 EV 도메인에서 0.5 EV를 썼는데, 우리 로그 축도 EV 도메인이라
  // 같은 개념이 그대로 적용된다.
  fx::Fraction forward_hysteresis  = fx::percent(60);	// 같은 방향 (민감)
  fx::Fraction reversal_hysteresis = fx::percent(90);	// 방향 전환 (둔감)

  uint32_t min_code_delta = 1;
  uint32_t reversal_min_code_delta = 2;					// 방향 전환 시 추가 요구
};

class QuantizationGuard
{
 public:
  QuantizationGuard(const LogExposureAxis& axis,
                    QuantizationGuardParams params = {})
      : axis_(axis), params_(params) {}

  // ------------------------------------------------------------
  // 동적 데드밴드 (Q8 밝기 단위)
  //
  // 두 하한을 함께 고려한다:
  //  (1) 로그 축 1스텝이 유발하는 밝기 변화 (상대)
  //  (2) EXPOSUREn 레지스터 정수 격자 1개가 유발하는 밝기 변화 (절대)
  //
  // (2)를 빼먹으면 짧은 노출에서 원리적으로 수렴 불가능해진다.
  //  예: EXPOSUREn=3 이면 격자 1개가 밝기 33% 변화인데 데드밴드가 2%면
  //      어떤 레지스터값을 써도 그 안에 못 들어가 조정만 무한 반복한다.
  // (libcamera RPi AGC가 락 판정에 "+200us"를 더하는 것과 같은 취지)
  // ------------------------------------------------------------
  Brightness dynamic_deadband(uint32_t code, Brightness target) const
  {
    // (1) 로그 스텝 기반 + 상/하한 clamp
    //로그축 한 칸의 변화율
    const Ratio log_step = axis_.nominal_step_ratio(code);

	//로그 스텝을 밝기 오차로 변환
	//target = 2240u * fx::kOne_q8 = 573,440
    Brightness deadband = fx::scale_q16(target, log_step);		//(target * log_step) >> 16

	//로그 데드밴드에 70% 적용
    deadband = fx::apply_fraction(deadband, params_.deadband_step_fraction);				//Deadband = Target x LogStepRatio x 0.70

    const Brightness min_deadband = fx::apply_fraction(target, params_.min_deadband);		//fx::percent(2);	// 0.02
    const Brightness max_deadband = fx::apply_fraction(target, params_.max_deadband);		//fx::percent(25);	// 0.25
    deadband = fx::clamp_u32(deadband, min_deadband, max_deadband);							//로그 데드밴드를 2~25%로 제한

    // (2) 레지스터 격자 기반 하한 (물리적 한계 -> 상한보다 우선)
    const Brightness grid_deadband = register_grid_deadband(code, target);
    return (deadband > grid_deadband) ? deadband : grid_deadband;
  }

  // 실제 1-register 변화율 = STEP / effective_time.
  // 레지스터 격자 1개가 유발하는 밝기 변화 기반 하한
  // 노출이 레지스터값에 선형 비례하므로 격자 1개 = 상대 1/reg 변화
  Brightness register_grid_deadband(uint32_t code, Brightness target) const
  {
    const ExposureUs effective = axis_.code_to_effective_time(code);		//현재 실효 노출시간 계산

    if (effective == 0) return target;

    //Register 한 스텝의 밝기 변화량
    //target x (1/reg) x fraction  -> 나눗셈 1회
    //OneStepChange = (Target x 10us STEP) / EffectiveTime )
    const Brightness one_step_change = static_cast<Brightness>(
        (static_cast<uint64_t>(target) * axis_.step_time()) / effective);

	//Register 격자 데드밴드 60%
    return fx::apply_fraction(one_step_change, params_.grid_deadband_fraction);
  }

  // 진단용: 이 구간의 데드밴드가 레지스터 격자에 지배되는가?
  // true면 하드웨어 해상도 한계이므로, 더 정밀하려면 게인 축 등이 필요하다.
  bool is_grid_limited(uint32_t code, Brightness target) const
  {
    const Ratio log_step = axis_.nominal_step_ratio(code);
    Brightness log_deadband = fx::scale_q16(target, log_step);
    log_deadband = fx::apply_fraction(log_deadband, params_.deadband_step_fraction);

	log_deadband = fx::clamp_u32(
        log_deadband,
        fx::apply_fraction(target, params_.min_deadband),
        fx::apply_fraction(target, params_.max_deadband));

    return register_grid_deadband(code, target) > log_deadband;
  }

  // ------------------------------------------------------------
  // 수렴 판정 (원칙 2)
  //   (a) 밝기가 동적 데드밴드 이내, 또는
  //   (b) 조정해도 같은 코드가 나옴 -> 더 나아질 여지 없음
  // ------------------------------------------------------------
  bool is_converged(uint32_t code,
                    Brightness measured,
                    Brightness target,
                    ExposureUs ideal_time) const
  {
    if (fx::abs_diff(measured, target) <= dynamic_deadband(code, target))
	{
      return true;
    }

    const uint32_t next_code = axis_.effective_time_to_code(ideal_time);
    return fx::abs_diff(next_code, code) < params_.min_code_delta;
  }

  // ------------------------------------------------------------
  // 방향성 히스테리시스를 적용한 코드 결정 (원칙 3)
  //
  // 진동은 본질적으로 "방향 전환의 반복"이므로, 전환에만 저항을 걸면
  // 수렴 속도를 거의 잃지 않고 헌팅만 선택적으로 억제할 수 있다.
  // ------------------------------------------------------------
  uint32_t resolve_code_with_hysteresis(uint32_t current_code,
                                        ExposureUs ideal_time)
  {
    const uint32_t candidate = axis_.effective_time_to_code(ideal_time);

    if (candidate == current_code)
		return current_code;

	//이동 방향 계산
    const int direction = (candidate > current_code) ? +1 : -1;

	//방향 전환 확인
    const bool reversal =
        (last_direction_ != 0) && (direction != last_direction_);

	//최소 이동량
    const uint32_t required_delta = reversal
        ? params_.reversal_min_code_delta			//방향 전환:2scode
        : params_.min_code_delta;					//같은방향/첫이동:1code

    if (fx::abs_diff(candidate, current_code) < required_delta)
	{
      return current_code;
    }

	//현재 코드와 후보 코드의 실제 시간
    const ExposureUs current_time = axis_.code_to_effective_time(current_code);
    const ExposureUs candidate_time = axis_.code_to_effective_time(candidate);
	//현재 위치와 후보 위치 사이의 전체 간격
    const ExposureUs span = fx::abs_diff(candidate_time, current_time);

    if (span == 0)
		return current_code;

    // 0 = 현재 코드 위치, Q16(=1.0) = 후보 코드 위치
    //Solver의 이상적인 시간이 현재 위치에서 이동한 거리
    const ExposureUs moved = fx::abs_diff(ideal_time, current_time);

	//같은 방향과 반대 방향의 임계값
    const fx::Fraction threshold = reversal
        ? params_.reversal_hysteresis		//90%::방향 전환
        : params_.forward_hysteresis;		//60%::같은 방향/첫이동

	//이동 비율이 임계값 이상
    if (!fx::fraction_at_least(moved, span, threshold))
	{// 아직 충분히 넘어오지 않음
      return current_code;
    }

    last_direction_ = direction;		// 실제로 움직였을 때만 방향 갱신

    return candidate;
  }

  void reset_direction()       { last_direction_ = 0; }
  int last_direction() const { return last_direction_; }

 private:
  const LogExposureAxis& axis_;
  QuantizationGuardParams params_;
  int last_direction_ = 0;
};

}  // namespace ae
