# shader

## add Functional

- [T#s whose levels reach past the full mip chain](../../../core/shader/recompiler/Optimization/src/ResourceMaterializer.cpp) - query results use the guest level count: spatial extents are 1 past the physical chain but within that count, and spatial/depth-or-layer extents are 0 past the count. Static specialization or runtime flags carry missing levels; image tables do not. Mip loads, LOD queries, loads whose base is unrepresented and table resinfo requiring missing-level metadata are refused. Packed BY2/BY4 loads there return zero. Tests cover MAX_MIP=LAST_LEVEL and desktop RDNA2 observations, not PS5 sampling/gather equivalence. The existing LAST_LEVEL/MAX_MIP decode clamp is retained; the reported LAST_LEVEL=9/MAX_MIP=0 discrepancy remains outside this fix. Closes when: all missing-level accesses and descriptor boundaries are measured on a PS5 and implemented.
