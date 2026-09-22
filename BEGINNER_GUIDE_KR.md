# 초보자용 MCAMv4 AE 코드 안내

## 1. 이 코드가 하는 일

카메라가 촬영한 Merge 전 Long RAW 밝기를 목표 밝기와 비교해 다음 Long
노출을 계산합니다. HDR Short 노출은 세션 시작 시 결정한 Ratio에서 만들며,
한 세션 안에서는 Ratio를 바꾸지 않습니다.

## 2. 처음 알아야 할 네 가지 값

| 이름 | 의미 | 내부 표현 |
|---|---|---|
| `Brightness` | Merge 전 Long RAW 밝기 | Q8 `uint32_t` |
| `ExposureUs` | 실효 노출시간 | Q8 us `uint32_t` |
| `Ratio` | 실행 중 변하는 정밀 비율 | Q16 `uint32_t` |
| `ExposurePair` | 센서 Long/Short Register | `uint16_t` 두 개 |

Q8은 실제 값에 256을 곱해 정수로 저장한다는 뜻입니다. 예를 들어 밝기
2240은 내부에서 `2240 × 256`입니다. 알고리즘 파일에서는 이 스케일을 직접
노출하지 않고 `Brightness`, `ExposureUs` 이름으로 읽을 수 있게 했습니다.

## 3. 실제 적분시간

센서 Register 값만으로 노출시간을 판단하면 33us가 빠집니다.

```text
실효시간 = Register × STEP + 33us
```

`ae_log_exposure_axis.hpp`가 다음 변환을 한 곳에서 담당합니다.

```text
로그 Code -> 명령시간 -> Register -> 실효시간
실효시간 -> Register -> 명령시간 -> 로그 Code
```

다른 알고리즘에서는 가능하면 이 변환 함수를 사용하고 33us를 다시 직접
더하거나 빼지 않아야 합니다.

## 4. 첫 번째 측정의 계산

현재 밝기가 목표보다 어두우면 노출을 늘립니다.

```text
다음 노출 = 현재 노출 × 목표 밝기 / 현재 밝기
```

예:

```text
현재 노출 = 563us
현재 밝기 = 1120
목표 밝기 = 2240

다음 노출 = 563 × 2240 / 1120
          = 1126us
```

이 계산은 `ae_solver.hpp`의 `first_shot_inversion()`에 있습니다.

## 5. 두 번째 측정부터의 계산

현재 세션에서 측정한 두 점을 이용해 Secant 방식으로 목표 노출을 구합니다.

```text
Tnext = T1 + (Target - Y1) × (T2 - T1) / (Y2 - Y1)
```

이전 세션의 밝기는 장면이 바뀌었을 수 있으므로 Secant 점으로 사용하지
않습니다. 웜스타트는 이전 Exposure Code만 시작점으로 재사용합니다.

## 6. 바로 적용하지 않고 Guard를 거치는 이유

센서 Register는 정수이므로 계산한 이상적 노출을 정확히 표현하지 못할 수
있습니다. `ae_quantization_guard.hpp`는 다음을 처리합니다.

- 하드웨어 한 스텝보다 작은 변화 무시
- 목표 근처의 작은 밝기 오차 허용
- 방향이 뒤집힐 때 더 큰 변화만 허용
- 같은 Register로 돌아가는 불필요한 촬영 방지

Register 한 스텝의 실제 상대 변화는 다음과 같이 계산합니다.

```text
STEP / (Register × STEP + 33us)
```

## 7. Zone 통계와 측광

`ae_zone_stats.cu`는 2048×2048 영상의 중앙 2040×2040을 12×12 Zone으로
나눕니다. 짝수 행 Long과 홀수 행 Short의 합, 픽셀 수, 포화 수, 암부 수를
각각 따로 계산합니다.

AE 밝기는 Merge 출력이 아니라 Merge 전 Long RAW에서 구합니다. Long AE의
유효 Zone 판정도 Long의 포화와 Long의 암부 통계만 사용합니다.

## 8. 코드를 읽는 순서

1. `example_usage.cpp`에서 객체 생성과 `run()` 호출 확인
2. `ae_manager_v2.hpp`에서 전체 순서 확인
3. `ae_solver.hpp`에서 두 노출 계산식 확인
4. `ae_quantization_guard.hpp`에서 수렴 판정 확인
5. `ae_scene_classifier.hpp`에서 Zone 선택 확인
6. `ae_log_exposure_axis.hpp`에서 시간과 Register 변환 확인
7. 마지막에 `ae_fixed.hpp`에서 정수 구현 확인

## 9. 실제 카메라 연결 전 점검표

- RAW 입력이 Linear 16bit 표현인지 확인
- 짝수 행이 Long이고 홀수 행이 Short인지 확인
- `STEP` 값이 실제 센서 설정과 일치하는지 확인
- `+33us`가 센서 ICD의 실제 적분시간 정의와 일치하는지 재확인
- `target_brightness`를 실제 RAW 데이터로 결정
- `short_target_us_q8`을 하이라이트 포화 데이터로 결정
- CUDA 커널의 입력 Width와 Buffer layout 확인
- 실제 장비에서 5 Shot 이내 수렴과 장면 급변을 반복 검증
