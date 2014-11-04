; This file tests support for the select instruction with vector valued inputs.

; RUN: %p2i -i %s --args -O2 --verbose none \
; RUN:   | llvm-mc -triple=i686-none-nacl -filetype=obj \
; RUN:   | llvm-objdump -d --symbolize -x86-asm-syntax=intel - | FileCheck %s
; RUN: %p2i -i %s --args -Om1 --verbose none \
; RUN:   | llvm-mc -triple=i686-none-nacl -filetype=obj \
; RUN:   | llvm-objdump -d --symbolize -x86-asm-syntax=intel - | FileCheck %s
; RUN: %p2i -i %s --args -O2 -mattr=sse4.1 --verbose none \
; RUN:   | llvm-mc -triple=i686-none-nacl -filetype=obj \
; RUN:   | llvm-objdump -d --symbolize -x86-asm-syntax=intel - \
; RUN:   | FileCheck --check-prefix=SSE41 %s
; RUN: %p2i -i %s --args -Om1 -mattr=sse4.1 --verbose none \
; RUN:   | llvm-mc -triple=i686-none-nacl -filetype=obj \
; RUN:   | llvm-objdump -d --symbolize -x86-asm-syntax=intel - \
; RUN:   | FileCheck --check-prefix=SSE41 %s

define <16 x i8> @test_select_v16i8(<16 x i1> %cond, <16 x i8> %arg1, <16 x i8> %arg2) {
entry:
  %res = select <16 x i1> %cond, <16 x i8> %arg1, <16 x i8> %arg2
  ret <16 x i8> %res
; CHECK-LABEL: test_select_v16i8:
; CHECK: pand
; CHECK: pandn
; CHECK: por

; SSE41-LABEL: test_select_v16i8:
; SSE41: pblendvb xmm{{[0-7]}}, {{xmm[0-7]|xmmword}}
}

define <16 x i1> @test_select_v16i1(<16 x i1> %cond, <16 x i1> %arg1, <16 x i1> %arg2) {
entry:
  %res = select <16 x i1> %cond, <16 x i1> %arg1, <16 x i1> %arg2
  ret <16 x i1> %res
; CHECK-LABEL: test_select_v16i1:
; CHECK: pand
; CHECK: pandn
; CHECK: por

; SSE41-LABEL: test_select_v16i1:
; SSE41: pblendvb xmm{{[0-7]}}, {{xmm[0-7]|xmmword}}
}

define <8 x i16> @test_select_v8i16(<8 x i1> %cond, <8 x i16> %arg1, <8 x i16> %arg2) {
entry:
  %res = select <8 x i1> %cond, <8 x i16> %arg1, <8 x i16> %arg2
  ret <8 x i16> %res
; CHECK-LABEL: test_select_v8i16:
; CHECK: pand
; CHECK: pandn
; CHECK: por

; SSE41-LABEL: test_select_v8i16:
; SSE41: pblendvb xmm{{[0-7]}}, {{xmm[0-7]|xmmword}}
}

define <8 x i1> @test_select_v8i1(<8 x i1> %cond, <8 x i1> %arg1, <8 x i1> %arg2) {
entry:
  %res = select <8 x i1> %cond, <8 x i1> %arg1, <8 x i1> %arg2
  ret <8 x i1> %res
; CHECK-LABEL: test_select_v8i1:
; CHECK: pand
; CHECK: pandn
; CHECK: por

; SSE41-LABEL: test_select_v8i1:
; SSE41: pblendvb xmm{{[0-7]}}, {{xmm[0-7]|xmmword}}
}

define <4 x i32> @test_select_v4i32(<4 x i1> %cond, <4 x i32> %arg1, <4 x i32> %arg2) {
entry:
  %res = select <4 x i1> %cond, <4 x i32> %arg1, <4 x i32> %arg2
  ret <4 x i32> %res
; CHECK-LABEL: test_select_v4i32:
; CHECK: pand
; CHECK: pandn
; CHECK: por

; SSE41-LABEL: test_select_v4i32:
; SSE41: pslld xmm0, 31
; SSE41: blendvps xmm{{[0-7]}}, {{xmm[0-7]|xmmword}}
}

define <4 x float> @test_select_v4f32(<4 x i1> %cond, <4 x float> %arg1, <4 x float> %arg2) {
entry:
  %res = select <4 x i1> %cond, <4 x float> %arg1, <4 x float> %arg2
  ret <4 x float> %res
; CHECK-LABEL: test_select_v4f32:
; CHECK: pand
; CHECK: pandn
; CHECK: por

; SSE41-LABEL: test_select_v4f32:
; SSE41: pslld xmm0, 31
; SSE41: blendvps xmm{{[0-7]}}, {{xmm[0-7]|xmmword}}
}

define <4 x i1> @test_select_v4i1(<4 x i1> %cond, <4 x i1> %arg1, <4 x i1> %arg2) {
entry:
  %res = select <4 x i1> %cond, <4 x i1> %arg1, <4 x i1> %arg2
  ret <4 x i1> %res
; CHECK-LABEL: test_select_v4i1:
; CHECK: pand
; CHECK: pandn
; CHECK: por

; SSE41-LABEL: test_select_v4i1:
; SSE41: pslld xmm0, 31
; SSE41: blendvps xmm{{[0-7]}}, {{xmm[0-7]|xmmword}}
}
