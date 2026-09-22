# MCAMv4 HDR Auto Exposure 전체 코드

이 폴더는 MCAMv4 HDR 카메라용 Auto Exposure 예제 전체를 포함합니다.
알고리즘 코드에서는 물리 의미가 먼저 보이도록 이름을 정리하고, Q-format과
정수 연산의 세부 구현은 하위 helper에 모았습니다. 소스 주석과 안내 문서는
한글로 작성했습니다.

## 먼저 읽을 파일

처음 보는 경우 아래 순서로 읽는 것이 가장 쉽습니다.

1. `BEGINNER_GUIDE_KR.md`: 전체 흐름과 핵심 수식
2. `FUNCTION_REFERENCE_KR.md`: 파일별 함수 역할 색인
3. `example_usage.cpp`: 실행 가능한 사용 예제
3. `mcamv4_ae/ae_manager_v2.hpp`: 한 세션의 전체 제어 순서
4. `mcamv4_ae/ae_solver.hpp`: 첫 Shot과 Secant 계산
5. `mcamv4_ae/ae_log_exposure_axis.hpp`: Code, Register, 실효시간 변환
6. `mcamv4_ae/ae_quantization_guard.hpp`: 데드밴드와 히스테리시스

`ae_fixed.hpp`는 정수 계산 구현을 모은 하위 파일이므로 마지막에 보는 것을
권장합니다.

## 연산 규칙

AE 소스는 다음 규칙을 따릅니다.

- `float`, `double`을 사용하지 않습니다.
- `exp()`, `pow()`, `sqrt()`, `log()`를 사용하지 않습니다.
- 기본 저장형은 `uint16_t`, `uint32_t`입니다.
- 곱셈 범위가 커지는 중간 계산에는 `uint64_t`를 사용합니다.
- 음수가 필요한 Secant 차분에는 `int64_t`를 사용합니다.
- 밝기와 노출시간은 Q8, 동적으로 변하는 비율은 Q16입니다.
- 고정 비율은 `7/10`, `60/100`처럼 Multiply/Divide 의미가 보이게 표현합니다.

규칙 검사는 다음 명령으로 실행합니다.

```bash
bash check_rules.sh
```

## 핵심 물리 모델

센서의 실제 적분시간은 다음 식으로 통일했습니다.

```text
실효 노출시간 = EXPOSUREn × STEP + 33us
```

따라서 첫 Shot, Secant, HDR Long/Short 계산, Quantization Guard가 모두
실효 노출시간을 사용합니다.

예를 들어 `EXPOSUREn=53`, `STEP=10us`이면:

```text
명령 시간 = 53 × 10us = 530us
실효 시간 = 530us + 33us = 563us
```

## Fixed Point 표현과 연산 선택

| 물리량 | 내부 표현 | 현재 범위 예시 |
|---|---|---|
| 밝기 | Q8 `uint32_t` | 4095×256=1,048,320 |
| 노출시간 | Q8 us `uint32_t` | 104,000×256=26,624,000 |
| 동적 비율 | Q16 `uint32_t` | Ratio 31은 31×65,536 |
| 중간 곱셈 | `uint64_t` | Q8/Q16 교차곱 오버플로 방지 |

고정된 60%, 70%, 90%는 `Fraction{num,den}`의 Multiply/Divide로 표현하고,
실행 중 변하는 정밀 Ratio만 Q16을 사용합니다. AE는 프레임당 한 번 수행되는
호스트 제어이므로 변수 분모 정수 나눗셈을 유지했습니다. 픽셀당 실행되는 GPU
Merge와 연산 빈도가 다르므로 동일한 reciprocal LUT 정책을 강제하지 않습니다.

Ratio 후보의 로그 거리 비교는 `log()` 대신 다음 관계를 사용합니다.

```text
ideal이 lo와 hi 사이일 때
ideal² < lo×hi  이면 lo가 로그 거리상 더 가깝다.
```

## AE 실행 흐름

```text
시작 Exposure 선택
  -> HDR Ratio를 세션당 한 번 결정
  -> 촬영
  -> 첫 프레임에서 Scene과 Weight 결정
  -> Merge 전 Long RAW 밝기 계산
  -> 첫 측정: Target/Current, 두 번째부터: Secant
  -> Deadband, Quantization, Hysteresis 적용
  -> 수렴 또는 최대 5 Shot까지 반복
```

웜스타트에서는 직전 세션의 Exposure Code만 시작값으로 사용합니다. 이전
세션의 밝기는 새 세션의 Secant 점으로 사용하지 않습니다.

## 빌드와 테스트

### CMake 사용

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/example_usage
```

### CMake 없이 g++ 사용

```bash
mkdir -p build
g++ -std=c++17 -Wall -Wextra -pedantic -I. self_test.cpp -o build/ae_self_test
g++ -std=c++17 -Wall -Wextra -pedantic -I. example_usage.cpp -o build/example_usage
./build/ae_self_test
./build/example_usage
```

## 파일 구성

| 파일 | 역할 |
|---|---|
| `example_usage.cpp` | Mock Camera를 이용한 전체 실행 예제 |
| `self_test.cpp` | 핵심 수식과 단위 변환 자체 테스트 |
| `check_rules.sh` | 금지된 실수형·함수·리터럴 검사 |
| `mcamv4_ae/ae_manager_v2.hpp` | AE 세션 전체 제어 |
| `mcamv4_ae/ae_solver.hpp` | Target/Current와 Secant 계산 |
| `mcamv4_ae/ae_log_exposure_axis.hpp` | Code/Register/시간 변환 |
| `mcamv4_ae/ae_hdr_ratio_schedule.hpp` | HDR Ratio 스케줄 |
| `mcamv4_ae/ae_quantization_guard.hpp` | 양자화 데드밴드·히스테리시스 |
| `mcamv4_ae/ae_scene_classifier.hpp` | Scene 판정과 Long RAW 측광 |
| `mcamv4_ae/ae_zone_stats.cu` | 12×12 Long/Short Zone 통계 CUDA 커널 |
| `mcamv4_ae/ae_camera_interface.hpp` | 실제 카메라 연동 인터페이스 |
| `mcamv4_ae/ae_fixed.hpp` | Fixed Point 공용 helper |

## 실제 장비에 연결할 때 구현할 부분

`ICameraCaptureInterface`를 상속한 클래스를 프로젝트에 구현해야 합니다.

1. `ExposurePair`의 `exposure1`, `exposure2`, `step`을 센서에 설정합니다.
2. HDR RAW 프레임을 촬영하고 다운로드합니다.
3. `ae_zone_stats.cu`를 실행해 144개 Zone 통계를 만듭니다.
4. 센서 온도와 성공 여부를 `CaptureResult`에 채웁니다.
5. `now_ms()`에서 시스템 시간을 반환합니다.

`example_usage.cpp`의 `MockCamera`가 인터페이스 구현 예시입니다.

### 현재 API 기준 통합 예

```cpp
ShutterLUT lut(kDefaultShutterTable, kDefaultShutterTableCount);

LogExposureAxisParams axis_params;
axis_params.code_min = 108u;
axis_params.code_max = 994u;
axis_params.max_nominal_time = 104000u * fx::kOne_q8;
axis_params.step = ExposureStep::kStep10us;
LogExposureAxis axis(lut, axis_params);

AEManagerV2 manager(camera, axis);
HdrRatioParams ratio_params;
ratio_params.short_target_us_q8 = 2000u * fx::kOne_q8;
manager.set_ratio_schedule(ratio_params);

AESessionConfig config;
config.target_brightness = 2240u * fx::kOne_q8;
const AESessionResult result = manager.run(config);
```

AE 세션은 동기식 초기화 단계로 실행한 뒤, `result.final_exposure`를 스트리밍
설정에 적용하는 구조를 기본 예로 봅니다. 실제 스레드·DAG 배치는 시스템의
TC/TM 지연과 오류 복구 요구사항을 포함해 결정해야 합니다.

## HDR Ratio Table 사용 시 주의

AR0233 원본 임계 테이블의 첫 번째 열은 `line` 단위입니다. MCAMv4의 AE는
Q8 `us` 단위를 사용하므로 원본 숫자를 직접 비교하면 안 됩니다.

- 기본값인 `kPinnedShort` 모드는 단위 혼용 없이 사용할 수 있습니다.
- `kThresholdTable` 모드는 MCAMv4 실효 노출시간(Q8 us)으로 변환한 테이블을
  `AEManagerV2::set_ratio_threshold_table()`로 전달한 경우에만 사용됩니다.
- 표가 없으면 원본 AR0233 line 값을 사용하지 않고 Pinned Short 계산으로
  자동 전환됩니다.

## 확인된 범위와 남은 검증

- C++17 헤더, 예제, 자체 테스트는 `g++`로 컴파일했습니다.
- 자체 테스트 11개가 모두 통과했습니다.
- 정수 연산 규칙 검사 결과 위반은 0건입니다.
- 현재 검증 환경에는 `cmake`와 CUDA `nvcc`가 없어 CMake 및 CUDA 실제
  컴파일은 확인하지 못했습니다.
- `target_brightness=2240`, `short_target=2000us`, Scene 분류 임계값은
  실제 MCAMv4 RAW 데이터로 튜닝해야 합니다.
- `max_nominal_time=104000us`는 현재 설정값입니다. ROI별 타이밍 예산과 실제
  센서 ICD를 대상 시스템에서 다시 확인해야 하며, 문서만으로 보장하지 않습니다.

## 코드 리뷰에서 보강한 오류 처리

- 빈 Long Zone을 유효 Zone 수와 평균에서 제외합니다.
- 촬영 3회 연속 실패 시 마지막 요청 Exposure를 결과에 보존합니다.
- 촬영 실패 세션은 Adaptive Damping 학습값을 변경하지 않습니다.
- 웜스타트 출력 포인터가 null이면 안전하게 false를 반환합니다.
- 명령시간→로그 코드 역변환은 기준 코드의 양쪽 후보를 확인합니다.
- Solver의 최대 변화 배율이 1.0 미만이면 안전하게 1.0으로 제한합니다.


---

## EGSE 실측 도구 (지상시험용)

AE에는 아직 실측으로 정해야 할 값이 있습니다. 그걸 재는 도구가 포함되어 있습니다.

**기본은 꺼져 있고, 비행 소프트웨어에는 들어가지 않습니다.**

```bash
cmake -DAE_ENABLE_EGSE_TOOLS=ON ..
make
./egse_tool all
python3 egse_analyze.py egse_result.csv
```

자세한 절차는 `EGSE_GUIDE_KR.md` 를 참고하세요.

| 항목 | 무엇을 재나 | 소요 |
|---|---|---|
| A | HDR + 12bpp 호환 | 5분 |
| B | 선형성 + 33us 검증 | 30분 |
| C | 블랙레벨 잔차 (렌즈캡) | 반나절 |
| D | 목표 밝기 확정 | 1~2시간 |
| E | 짧은 노출 목표 확정 | 1시간 |
| F | 노출 반영 지연 | 15분 |
