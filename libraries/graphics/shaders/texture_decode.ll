; Native texture import: encoded source storage -> GPU-resident linear texels.
; Header: count, width, base, pitch, indexed, source-start, palette[256], words[].
target triple = "spirv-unknown-vulkan1.3"
@source_name = private constant [7 x i8] c"source\00"
@output_name = private constant [7 x i8] c"output\00"
declare spir_func i32 @nt_gpu_texture_address(i32, i32, i32, i32, i32)
declare i32 @llvm.spv.thread.id.i32(i32)
declare target("spirv.VulkanBuffer", [0 x i32], 12, 0) @llvm.spv.resource.handlefrombinding.tspirv.VulkanBuffer_a0i32_12_0t(i32, i32, i32, i32, ptr)
declare target("spirv.VulkanBuffer", [0 x i32], 12, 1) @llvm.spv.resource.handlefrombinding.tspirv.VulkanBuffer_a0i32_12_1t(i32, i32, i32, i32, ptr)
declare ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0), i32)
declare ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_1t(target("spirv.VulkanBuffer", [0 x i32], 12, 1), i32)
define void @main() #0 {
entry:
  %input = call target("spirv.VulkanBuffer", [0 x i32], 12, 0) @llvm.spv.resource.handlefrombinding.tspirv.VulkanBuffer_a0i32_12_0t(i32 0, i32 0, i32 1, i32 0, ptr @source_name)
  %output = call target("spirv.VulkanBuffer", [0 x i32], 12, 1) @llvm.spv.resource.handlefrombinding.tspirv.VulkanBuffer_a0i32_12_1t(i32 0, i32 1, i32 1, i32 0, ptr @output_name)
  %countp = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 0)
  %count = load i32, ptr addrspace(11) %countp, align 4
  %id = call i32 @llvm.spv.thread.id.i32(i32 0)
  %active = icmp ult i32 %id, %count
  br i1 %active, label %convert, label %done
convert:
  %widthp = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 1)
  %basep = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 2)
  %pitchp = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 3)
  %indexedp = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 4)
  %startp = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 5)
  %width = load i32, ptr addrspace(11) %widthp, align 4
  %base = load i32, ptr addrspace(11) %basep, align 4
  %pitch = load i32, ptr addrspace(11) %pitchp, align 4
  %indexed = load i32, ptr addrspace(11) %indexedp, align 4
  %start = load i32, ptr addrspace(11) %startp, align 4
  %x = urem i32 %id, %width
  %y = udiv i32 %id, %width
  %address = call spir_func i32 @nt_gpu_texture_address(i32 %base, i32 %pitch, i32 %x, i32 %y, i32 %indexed)
  %word = lshr i32 %address, 3
  %relative0 = sub i32 %word, %start
  %relative = and i32 %relative0, 1048575
  %source_index = add i32 %relative, 262
  %sourcep = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 %source_index)
  %source = load i32, ptr addrspace(11) %sourcep, align 4
  %nibble = and i32 %address, 7
  %shift = shl i32 %nibble, 2
  %shifted = lshr i32 %source, %shift
  %eight = icmp eq i32 %indexed, 2
  %index_mask = select i1 %eight, i32 255, i32 15
  %palette_index0 = and i32 %shifted, %index_mask
  %palette_index = add i32 %palette_index0, 6
  %palettep = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_0t(target("spirv.VulkanBuffer", [0 x i32], 12, 0) %input, i32 %palette_index)
  %palette = load i32, ptr addrspace(11) %palettep, align 4
  %use_palette = icmp ne i32 %indexed, 0
  %color = select i1 %use_palette, i32 %palette, i32 %source
  %outp = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11.tspirv.VulkanBuffer_a0i32_12_1t(target("spirv.VulkanBuffer", [0 x i32], 12, 1) %output, i32 %id)
  store i32 %color, ptr addrspace(11) %outp, align 4
  br label %done
done:
  ret void
}
attributes #0 = { "hlsl.numthreads"="64,1,1" "hlsl.shader"="compute" }
