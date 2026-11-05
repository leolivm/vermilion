; ModuleID = 'nvmtl-air'
source_filename = "tg_half4_min"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

%struct.tg = type { %"struct.metal::array.32" }
%"struct.metal::array.32" = type { [128 x <4 x half>] }

define void @k(i32 noundef %tid, ptr addrspace(1) noundef "air-buffer-no-alias" %out, ptr addrspace(3) noundef "air-buffer-no-alias" %tg) local_unnamed_addr #0 {
entry:
  %m = getelementptr inbounds %struct.tg, ptr addrspace(3) %tg, i64 0, i32 0
  %i = zext i32 %tid to i64
  %p = getelementptr inbounds %"struct.metal::array.32", ptr addrspace(3) %m, i64 0, i32 0, i64 %i
  store <4 x half> <half 0xH3C00, half 0xH3C00, half 0xH3C00, half 0xH3C00>, ptr addrspace(3) %p, align 8
  tail call void @air.wg.barrier(i32 2, i32 1)
  %j = xor i64 %i, 1
  %q = getelementptr inbounds %"struct.metal::array.32", ptr addrspace(3) %m, i64 0, i32 0, i64 %j
  %v = load <4 x half>, ptr addrspace(3) %q, align 8
  %o = getelementptr inbounds <4 x half>, ptr addrspace(1) %out, i64 %i
  store <4 x half> %v, ptr addrspace(1) %o, align 8
  ret void
}

declare void @air.wg.barrier(i32, i32) local_unnamed_addr #1

attributes #0 = { convergent mustprogress nounwind "max-work-group-size"="128" }
attributes #1 = { convergent mustprogress nounwind willreturn }

!llvm.module.flags = !{!0, !1, !2}
!air.version = !{!3}
!air.language_version = !{!4}
!air.kernel = !{!5}

!0 = !{i32 7, !"air.max_device_buffers", i32 31}
!1 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!2 = !{i32 7, !"air.max_constant_buffers", i32 31}
!3 = !{i32 2, i32 8, i32 0}
!4 = !{!"Metal", i32 4, i32 0, i32 0}
!5 = !{ptr @k, !6, !7, !13}
!6 = !{}
!7 = !{!8, !9, !10}
!8 = !{i32 0, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!9 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"half4", !"air.arg_name", !"out"}
!10 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.struct_type_info", !11, !"air.arg_type_size", i32 1024, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"tg", !"air.arg_name", !"tg"}
!11 = !{!"air.struct_type_info", !12, i32 0, i32 1024, i32 0, !"metal::array", !"m"}
!12 = !{i32 0, i32 8, i32 128, !"half4", !"__elems"}
!13 = !{!"air.max_work_group_size", i32 128}
