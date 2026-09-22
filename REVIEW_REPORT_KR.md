# 전체 코드 재검토 결과

## 적용한 수정

1. 모든 AE 시간 계산을 `Register × STEP + 33us` 실효시간으로 통일했습니다.
2. 첫 측정은 `Target/Current`, 두 번째부터는 현재 세션의 두 점으로 Secant를
   계산하도록 했습니다.
3. 웜스타트에서 이전 세션의 밝기를 새 Secant 이력으로 사용하지 않도록
   수정했습니다.
4. HDR Short Register를 Long 실효시간과 Ratio에서 계산하도록 했습니다.
5. Long/Short 포화와 암부 통계를 분리하고 Long AE는 Long 통계만 사용합니다.
6. Quantization Guard의 실제 한 스텝을 `STEP/실효시간`으로 계산합니다.
7. AR0233 line 임계표와 MCAMv4 Q8 us의 단위 혼용을 차단했습니다.
8. 알고리즘 코드에서는 도메인 이름을 사용하고 Q-format 상세를 helper로
   이동했습니다.
9. 한글 설명 문서와 11개 자체 테스트를 포함했습니다.
10. 빈 Zone 유효 처리, 촬영 실패 결과, 실패 세션 Damping, 역변환 후보 검색을
    코드 리뷰 후 보강했습니다.
11. 모든 함수 역할을 찾을 수 있는 `FUNCTION_REFERENCE_KR.md`를 추가했습니다.

## 연산 규칙 검사

검사 대상은 모든 `.hpp`, `.cu`, `example_usage.cpp`, `self_test.cpp`입니다.

| 항목 | 결과 |
|---|---|
| `float` | 0건 |
| `double` | 0건 |
| `exp()` / `pow()` | 0건 |
| `sqrt()` / `log()` | 0건 |
| 소수점 실수 리터럴 | 0건 |
| 지수형 실수 리터럴 | 0건 |

Secant 차분에는 음수가 필요하므로 `int64_t`를 사용합니다. C++의 `main()`과
반복 횟수·방향 등 음수 또는 표준 반환형이 필요한 곳에는 `int` 계열이 남아
있습니다. 이를 unsigned로 강제하면 underflow 또는 인터페이스 문제가 생깁니다.

## 실행 검증

검증 명령:

```bash
g++ -std=c++17 -Wall -Wextra -pedantic -I. self_test.cpp -o build/ae_self_test
g++ -std=c++17 -Wall -Wextra -pedantic -I. example_usage.cpp -o build/example_usage
./build/ae_self_test
./build/example_usage
./check_rules.sh
```

결과:

- 두 C++ 파일 모두 경고 없이 컴파일되었습니다.
- 자체 테스트 11개가 모두 통과했습니다.
- Mock Camera 예제가 정상 종료했습니다.
- `-Wconversion` 추가 검사와 UndefinedBehaviorSanitizer 실행도 통과했습니다.
- 금지 연산 검사는 위반 0건으로 종료했습니다.

## 현재 환경에서 확인하지 못한 항목

현재 환경에는 `cmake`와 CUDA `nvcc`가 없습니다. 따라서 다음 항목은 대상
AGX/CUDA 환경에서 추가 검증이 필요합니다.

- CMake 생성 및 CTest 실행
- `ae_zone_stats.cu`의 실제 CUDA 컴파일
- AGX Orin의 CUDA Architecture 87 빌드
- 실제 SpaceWire/TC/TM 카메라 연동
- 실제 RAW를 사용한 목표 밝기와 Scene 임계값 튜닝

확인하지 못한 부분을 임의로 통과했다고 표시하지 않았습니다.
