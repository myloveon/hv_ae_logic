// ae_adaptive_damping.hpp
// 부호반전 기반 적응형 감쇠.
//
// 부호반전(sign reversal) 기반 적응형 감쇠. 정수 전용.
//
// 한 세션 안에서 노출 조정 방향이 반복해서 바뀌는 현상을 헌팅의 간단한
// 지표로 사용한다. reversal_threshold는 실제 장비 데이터로 검증해야 한다.
// 고정된 0.70 / 1.15 같은 값은 Q16 숫자를 노출하지 않고 분수로 표현한다.

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"
#include "ae_types.hpp"

namespace ae {

struct AdaptiveDampingParams
{
  int reversal_threshold = 2;					   // 이 이상 뒤집히면 헌팅 판정
  fx::Fraction tighten = fx::fraction(7, 10);      // 헌팅 시 x0.70 (조임)
  fx::Fraction relax   = fx::fraction(115, 100);   // 안정 시 x1.15 (완화)
  Ratio min_step_ratio = fx::q16_ratio(13, 10);    // 하한 x1.30
  Ratio max_step_ratio = fx::q16_ratio(4, 1);      // 상한 x4.00
};

class AdaptiveDamping
{
 public:
  // 역할: 감쇠 파라미터와 첫 Q16 최대 변화 배율을 저장한다.
  explicit AdaptiveDamping(AdaptiveDampingParams params = {},
                            Ratio initial = fx::q16_ratio(2, 1))
      : params_(params), step_ratio_(initial) {}

  // 역할: 새 세션의 반전 관측값을 초기화하고 학습된 배율은 유지한다.
  void begin_session()
  {
    reversals_ = 0;
    previous_direction_ = 0;
  }

  // 역할: 실제 노출 이동 방향(+1/-1/0)을 기록하고 반전 횟수를 센다.
  void observe(int direction)
  {
    if (direction == 0) return;

    if (previous_direction_ != 0 && direction != previous_direction_)
	{
      ++reversals_;
    }
    previous_direction_ = direction;
  }

  // 역할: 정상 종료 세션의 반전 횟수로 다음 세션 변화 배율을 갱신한다.
  void end_session()
  {
    const fx::Fraction adjustment = hunting_detected()
        ? params_.tighten
        : params_.relax;

    step_ratio_ = fx::apply_fraction(step_ratio_, adjustment);
    step_ratio_ = fx::clamp_u32(step_ratio_,
                                params_.min_step_ratio,
                                params_.max_step_ratio);
  }

  // 역할: 실패 세션 관측값을 버리고 기존 학습 배율을 유지한다.
  void cancel_session()
  {
    reversals_ = 0;
    previous_direction_ = 0;
  }

  // 반환: 다음 웜스타트 세션에 적용할 Q16 최대 변화 배율.
  Ratio step_ratio() const { return step_ratio_; }
  // 반환: 현재 세션의 방향 반전 횟수.
  int reversals_this_session() const { return reversals_; }
  // 반환: 반전 횟수가 설정 임계값 이상이면 true.
  bool hunting_detected() const { return reversals_ >= params_.reversal_threshold; }

 private:
  AdaptiveDampingParams params_;
  Ratio step_ratio_;
  int reversals_ = 0;
  int previous_direction_ = 0;
};

}  // namespace ae
