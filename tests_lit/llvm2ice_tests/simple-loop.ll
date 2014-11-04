; This tests a simple loop that sums the elements of an input array.
; The O2 check patterns represent the best code currently achieved.

; RUN: %p2i -i %s --args -O2 --verbose none \
; RUN:   | llvm-mc -triple=i686-none-nacl -filetype=obj \
; RUN:   | llvm-objdump -d -symbolize -x86-asm-syntax=intel - | FileCheck %s
; RUN: %p2i -i %s --args -Om1 --verbose none \
; RUN:   | llvm-mc -triple=i686-none-nacl -filetype=obj \
; RUN:   | llvm-objdump -d -symbolize -x86-asm-syntax=intel - \
; RUN:   | FileCheck --check-prefix=OPTM1 %s

define i32 @simple_loop(i32 %a, i32 %n) {
entry:
  %cmp4 = icmp sgt i32 %n, 0
  br i1 %cmp4, label %for.body, label %for.end

for.body:
  %i.06 = phi i32 [ %inc, %for.body ], [ 0, %entry ]
  %sum.05 = phi i32 [ %add, %for.body ], [ 0, %entry ]
  %gep_array = mul i32 %i.06, 4
  %gep = add i32 %a, %gep_array
  %__9 = inttoptr i32 %gep to i32*
  %v0 = load i32* %__9, align 1
  %add = add i32 %v0, %sum.05
  %inc = add i32 %i.06, 1
  %cmp = icmp slt i32 %inc, %n
  br i1 %cmp, label %for.body, label %for.end

for.end:
  %sum.0.lcssa = phi i32 [ 0, %entry ], [ %add, %for.body ]
  ret i32 %sum.0.lcssa
}

; CHECK-LABEL: simple_loop
; CHECK:      mov ecx, dword ptr [esp{{.*}}+{{.*}}{{[0-9]+}}]
; CHECK:      cmp ecx, 0
; CHECK-NEXT: j{{le|g}} {{[0-9]}}

; Check for the combination of address mode inference, register
; allocation, and load/arithmetic fusing.
; CHECK:      add e{{..}}, dword ptr [e{{..}} + 4*[[IREG:e..]]]
; Check for incrementing of the register-allocated induction variable.
; CHECK-NEXT: add [[IREG]], 1
; Check for comparing the induction variable against the loop size.
; CHECK-NEXT: cmp [[IREG]],
; CHECK-NEXT: jl -{{[0-9]}}
;
; There's nothing remarkable under Om1 to test for, since Om1 generates
; such atrocious code (by design).
; OPTM1-LABEL: simple_loop
; OPTM1:      cmp {{.*}}, 0
; OPTM1:      jg
; OPTM1:      ret
