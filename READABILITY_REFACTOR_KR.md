# Fixed-Point AE 가독성 리팩터링 원칙

## 목표

연산 규칙은 그대로 유지한다.

- 실수형 사용 금지
- 지수/거듭제곱 함수 사용 금지
- uint16_t / uint32_t 중심
- 곱셈 중간값은 필요 시 uint64_t
- 동적 정밀 비율은 Q-format
- 고정 상수 분수는 Multiply / Divide
- LUT / Multiply + Shift 우선

단, 알고리즘 파일에서는 Q-format 구현 상세가 먼저 보이지 않게 한다.

## 1. 물리 의미 타입 별칭

```cpp
using Brightness   = uint32_t;  // Q8
using ExposureUs   = uint32_t;  // Q8 us
using Ratio        = uint32_t;  // Q16
using TemperatureC = int32_t;   // Q8 C
```

따라서 아래보다

```cpp
uint32_t cur_us_q8;
uint32_t bri_q8;
uint32_t ratio_q16;
```

다음을 권장한다.

```cpp
ExposureUs current_time;
Brightness current_brightness;
Ratio step_ratio;
```

Q-format은 타입 선언부에서 한 번만 확인하면 된다.

## 2. 고정 분수는 Q16 숫자로 노출하지 않는다

기존:

```cpp
uint32_t forward_hysteresis_q16 = 39322;
uint32_t reversal_hysteresis_q16 = 58982;
```

권장:

```cpp
fx::Fraction forward_hysteresis  = fx::percent(60);
fx::Fraction reversal_hysteresis = fx::percent(90);
```

실제 연산은 정수 교차곱 또는 Multiply/Divide다.

## 3. Q16은 동적으로 변하는 비율에만 사용

예: Adaptive Damping의 현재 최대 step ratio는 세션마다 1.30~4.00 사이에서 변한다.
이 값은 Q16 유지가 적합하다.

반면 70%, 60%, 90%처럼 고정된 튜닝값은 Fraction이 더 읽기 쉽다.

## 4. 가중치는 Q16일 필요가 없다

Scene weight 0.05 / 1.0 / 1.5는 weighted average에서 공통 스케일이 소거된다.
따라서 다음과 같이 사용한다.

```cpp
suppressed_weight = 5;
normal_weight     = 100;
emphasized_weight = 150;
```

계산:

```cpp
weighted_sum += zone.mean_long() * weight;
total_weight += weight;
mean = weighted_sum / total_weight;
```

실수 연산 없이 의미가 바로 보인다.

## 5. 노출시간 도메인은 이름으로 분리

MCAMv4는 +33us 때문에 두 시간이 다르다.

- nominal time: LUT가 표현하는 sensor command time
- effective time: 실제 적분시간 = register * STEP + 33us

따라서 `code_to_us_q8()`처럼 모호한 이름 대신 다음을 사용한다.

```cpp
axis.code_to_nominal_time(code);
axis.code_to_effective_time(code);
axis.effective_time_to_register(time);
```

AE Target/Current, Secant, HDR ratio는 effective time을 사용한다.

## 6. Long/Short 통계는 명시적으로 분리

```cpp
cnt_long_saturated
cnt_long_dark
cnt_short_saturated
cnt_short_dark
```

Long 기반 AE valid-zone 판정은 Long 통계만 사용한다.

## 7. 알고리즘 본문 예

기존 스타일:

```cpp
const uint32_t cur_us_q8 = axis_.code_to_us_q8(code);
const uint32_t bri = (met.weighted_mean_q8 > residual)
                         ? (met.weighted_mean_q8 - residual) : 0u;
ideal_us_q8 = solver.secant_step(prev_us_q8, prev_bri_q8,
                                  cur_us_q8, bri,
                                  cfg.target_brightness_q8);
```

리팩터링 스타일:

```cpp
const ExposureUs current_time = axis_.code_to_effective_time(code);
const Brightness current_brightness =
    (metering.weighted_mean > black_residual)
        ? (metering.weighted_mean - black_residual)
        : 0u;

const ExposureUs ideal_time = solver.secant_step(
    previous_time,
    previous_brightness,
    current_time,
    current_brightness,
    target_brightness);
```

동일하게 정수 연산만 사용하지만 알고리즘 흐름이 먼저 보인다.

## 검증

- C++17 example 빌드 성공
- check_rules.sh: 금지 연산 0건
- +33us를 포함한 effective exposure domain 사용
- Long/Short saturation/dark 통계 분리
