# Render-stream integration staging

This commit stages the Vulkan compute-to-vertex recorder and a headless graphics test source on the isolated research branch. The sources depend on headers, planners, shader files, CMake registration and test fixtures that are not in this commit; they are deliberately not wired into the normal build. The remaining source overlay is supplied separately and must be reviewed, integrated and validated before enabling the target.

The intended data path is AOT-generated SPIR-V compute -> persistent GPU vertex buffer -> Vulkan vertex input and draw, with a compute-write to vertex-read barrier and no intermediate host readback. The headless test includes diagnostic readback *after* drawing, which is not part of the production path.

Not yet verified: full LLVM SPIR-V compilation, Vulkan graphics execution, Windows build, and GT4 menu integration. No compatibility or speedup claim is made. Do not enable in production until the complete source overlay is present, the repository content guard passes and the GPU validation tests pass.
