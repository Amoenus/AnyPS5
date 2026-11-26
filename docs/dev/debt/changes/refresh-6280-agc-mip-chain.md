# libSceAgcDriver

## add Functional

- [Mip levels past the full chain](../../../core/libs/prx/libSceAgcDriver/Graphics/src/Texture.cpp) - images allocate only the physical mip chain while guest layouts retain all descriptor levels. Sampled views and MIN_LOD clamp to the last physical level, whose texels stand in for the unrepresented levels; those levels are not uploaded. Storage views and color attachments outside the physical chain throw. Whole-surface copy/fill fast paths refuse these surfaces, and tracking is per layer so unused extra-level bytes are preserved. This does not implement distinct texels or establish console sampling/gather behavior past the chain. Closes when: the extra guest levels are represented and checked against console results.
