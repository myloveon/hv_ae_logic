// ae_solver.hpp
//
// AE 핵심 계산. 알고리즘 본문에서는 물리 의미가 먼저 보이도록
// Brightness / ExposureUs 별칭을 사용한다. 실제 Q-format은 ae_types.hpp에 숨긴다.

// AE 핵심 계산 (정수 전용). "다음에 어떤 노출을 시도할지"만 결정한다.
// 실제 촬영/다운로드는 ae_manager가 담당한다(관심사 분리).
//
// ── 알고리즘 ────────────────────────────────────────────────
//  1샷  : 선형 역산   ev_next = ev * target / measured
//  2샷~ : Secant(할선법). 나눗셈을 한 번만 하도록 교차곱 형태로 정리한다.
//           ev_next = ev1 + (target - I1) x (ev2 - ev1) / (I2 - I1)
//         기울기를 따로 구하지 않으므로 정밀도 손실과 연산이 모두 줄어든다.
//
// ── 브라케팅 실패 시 처리 (중요) ────────────────────────────
//  과거 구현은 목표가 두 실측점 "바깥"일 때 이분법(기하평균)으로 폴백했는데,
//  이는 심각한 버그였다. 이분법은 목표를 사이에 둔 상태에서만 유효하다.
//  콜드스타트처럼 목표가 바깥이면 기하평균은 두 점 사이로 가버려 오히려
//  목표에서 멀어지고, 결과적으로 무한 왕복 진동에 빠진다.
//    예: 221 -> 881 -> 442 -> 1762 -> 881 (수렴 실패)
//  따라서 바깥이면 그 방향으로 나아가야(외삽) 하며, rate limit으로만 제한한다.

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"
#include "ae_types.hpp"

namespace ae {

class ExposureSolver
{
 public:
  // 역할: 수렴 허용치와 1회 변화 제한을 저장한다.
  // 주의: 최대 변화 배율이 1.0 미만이면 1.0으로 보정한다.
  explicit ExposureSolver(const AEAlgoParams& params) : params_(params)
  {
    if (params_.max_step_ratio < fx::kOne_q16)
	{
      params_.max_step_ratio = fx::kOne_q16;
    }
  }

  // 역할: 고정 tolerance 기준의 단순 수렴 여부를 계산한다.
  bool is_converged(Brightness measured, Brightness target) const
  {
    const Brightness tolerance = fx::scale_q16(target, params_.tolerance);
    return fx::abs_diff(measured, target) <= tolerance;
  }

  // 역할: 첫 측정에서 Tnext=Tcur*Target/Measured로 다음 노출을 역산한다.
  // 주의: 입력 RAW가 선형화되어 있다는 전제이며 실제 데이터 검증이 필요하다.
  ExposureUs first_shot_inversion(ExposureUs current,
                                  Brightness measured,
                                  Brightness target) const
  {
    // 측정값이 지나치게 작으면(암부 클리핑 근접) 큰 스텝으로 대체
    if (measured < fx::kOne_q8)
	{
      return clamp_step(current, fx::scale_q16(current, params_.max_step_ratio));
    }

	//Ideal=Current×Target/Measured :: 시간 × 밝기 / 밝기 = 시간
    const ExposureUs ideal = static_cast<ExposureUs>(
        (static_cast<uint64_t>(current) * target) / measured);

    return clamp_step(current, ideal);
  }

  // 역할: 현재 세션의 두 실측점으로 Secant 노출을 계산한다.
  // 계산: Tnext=T1+((Target-Y1)*(T2-T1)/(Y2-Y1))
  ExposureUs secant_step(ExposureUs t1, Brightness y1,
                         ExposureUs t2, Brightness y2,
                         Brightness target) const
  {
    const int64_t delta_y = static_cast<int64_t>(y2) - static_cast<int64_t>(y1); 	//밝기 변화량 : ΔY=Y2​−Y1​s

    if (delta_y == 0)
    {//밝기 변화가 없음
		return fallback_large_step(t2, y2, target);
    }

    // 두 실측점의 밝기 차가 거의 없으면 기울기를 신뢰할 수 없다
    // (예: 두 노출 모두 포화되어 4095로 클리핑된 경우)
    // -> 방향만 보고 큰 폭으로 이동
    const int64_t delta_t = static_cast<int64_t>(t2) - static_cast<int64_t>(t1);	//노출시간 변화량 : ΔT=T2​−T1​
    if (delta_t == 0)
    {//밝기 변화가 없음
		return fallback_large_step(t2, y2, target);
    }

    // 노출을 늘렸는데 밝기가 줄었거나 그 반대면 현재 2점의 선형 모델을 신뢰하지 않는다.
    if ((delta_t > 0 && delta_y < 0)		//노출 증가, 밝기 감소
		|| (delta_t < 0 && delta_y > 0))	//노출 감소, 밝기 증가
	{
      return fallback_large_step(t2, y2, target);
    }

	//목표 오차
    const int64_t target_error =
        static_cast<int64_t>(target) - static_cast<int64_t>(y1);

    // 시간 = 밝기 × Δ시간 / Δ밝기
    const int64_t correction = (target_error * delta_t) / delta_y;
	//다음 노출시간
    const int64_t next = static_cast<int64_t>(t1) + correction;

    if (next <= 0)
    {//노출시간은 양수
		return fallback_large_step(t2, y2, target);
    }

	//t2를 기준으로 최대 변화 범위 안에 제한
    return clamp_step(t2, static_cast<ExposureUs>(next));
  }

  // 역할: 유효 Zone 부족/기울기 오류에서 방향만 보고 제한된 큰 폭으로 이동한다.
  ExposureUs fallback_large_step(ExposureUs current,
                                 Brightness measured,
                                 Brightness target) const
  {
    if (measured == target)		//현재 밝기와 목표가 같음
		return current;

    return (measured < target)
        ? fx::scale_q16(current, params_.max_step_ratio)		//현재가 목표보다 어두움 :노출을 최대 변화 배율만큼 증가
        : fx::unscale_q16(current, params_.max_step_ratio);		//현재가 목표보다 밝음 : 노출을 최대 변화 배율만큼 감소
  }

 private:
  // 역할: 이상적 노출을 현재/max_ratio~현재*max_ratio 범위로 제한한다.
  ExposureUs clamp_step(ExposureUs current, ExposureUs ideal) const
  {
    const ExposureUs upper = fx::scale_q16(current, params_.max_step_ratio);	//Upper=Current×MaxRatio
    const ExposureUs lower = fx::unscale_q16(current, params_.max_step_ratio);	//Lower=Current/MaxRatio
    return fx::clamp_u32(ideal, lower, upper);
  }

  AEAlgoParams params_;
};

}  // namespace ae
