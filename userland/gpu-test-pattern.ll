; SPDX-License-Identifier: MIT
; Owned AIR fixture. Metadata follows the pinned metal2vulkan AIR contract;
; llvm-as generates bitcode locally. No Apple compiler output is embedded.
source_filename = "diting-gpu-test"
target datalayout = "e-p:64:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"

define <{ <4 x float> }> @diting_test_vertex(i32 %vertex) {
  %a = shl i32 %vertex, 1
  %b = and i32 %a, 2
  %c = and i32 %vertex, 2
  %x0 = uitofp i32 %b to float
  %y0 = uitofp i32 %c to float
  %x1 = fmul float %x0, 2.0
  %y1 = fmul float %y0, 2.0
  %x = fadd float %x1, -1.0
  %y = fadd float %y1, -1.0
  %v0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %x, i32 0
  %v1 = insertelement <4 x float> %v0, float %y, i32 1
  %out = insertvalue <{ <4 x float> }> undef, <4 x float> %v1, 0
  ret <{ <4 x float> }> %out
}
define <4 x float> @diting_test_fragment(<4 x float> %position, ptr addrspace(2) %color) {
  %out = load <4 x float>, ptr addrspace(2) %color, align 16
  ret <4 x float> %out
}
define void @diting_test_compute(ptr addrspace(1) %output, i32 %index) {
  %a = mul i32 %index, 1664525
  %b = add i32 %a, 1013904223
  %c = xor i32 %b, 1515870810
  %p = getelementptr i32, ptr addrspace(1) %output, i32 %index
  store i32 %c, ptr addrspace(1) %p, align 4
  ret void
}
!air.vertex = !{!0}
!air.fragment = !{!4}
!air.kernel = !{!9}
!air.version = !{!14}
!air.language_version = !{!15}
!0 = !{ptr @diting_test_vertex, !1, !2}
!1 = !{!3}
!2 = !{!16}
!3 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!4 = !{ptr @diting_test_fragment, !5, !6}
!5 = !{!7}
!6 = !{!8, !17}
!7 = !{!"air.render_target", i32 0, !"air.arg_type_name", !"float4"}
!8 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!9 = !{ptr @diting_test_compute, !10, !11}
!10 = !{}
!11 = !{!12, !13}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"output"}
!13 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"index"}
!14 = !{i32 2, i32 8, i32 0}
!15 = !{!"Metal", i32 3, i32 1, i32 0}
!16 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vertex"}
!17 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
