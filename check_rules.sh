#!/bin/bash

# AE 소스에서 실수형과 금지된 수학 연산을 찾는다.
# 주석은 검사에서 제외하며, 헤더/CUDA/예제/자체 테스트를 모두 검사한다.
set -u

echo "=== AE 정수 연산 규칙 검사 ==="

violation_count=0
pattern='\b(float|double)\b|\b(exp|pow|sqrt|log)[[:space:]]*\(|std::abs|\b[0-9]+\.[0-9]*([eE][+-]?[0-9]+)?[fFlL]?\b|\b[0-9]+[eE][+-]?[0-9]+[fFlL]?\b'

# EGSE 도구도 카메라와 같은 정수 규칙을 지켜야 한다.
# (분석용 egse_analyze.py 는 PC에서 도는 도구라 검사 대상이 아니다)
for source_file in mcamv4_ae/*.hpp mcamv4_ae/*.cu example_usage.cpp self_test.cpp egse_tool.cpp; do
  # 현재 소스는 줄 단위(//) 주석을 사용한다. 주석 뒤의 설명용 소수는 제거된다.
  violations=$(sed 's://.*::' "$source_file" | grep -nE "$pattern" || true)
  if [ -n "$violations" ]; then
    count=$(printf '%s\n' "$violations" | wc -l)
    echo "[위반 ${count}] ${source_file}"
    printf '%s\n' "$violations"
    violation_count=$((violation_count + count))
  else
    echo "[통과] ${source_file}"
  fi
done

if [ "$violation_count" -ne 0 ]; then
  echo "총 위반: ${violation_count}"
  exit 1
fi

echo "[통과] 금지된 실수 연산/리터럴이 없습니다."
echo "검사 대상: 헤더, CUDA 소스, 예제, 자체 테스트, EGSE 도구"
