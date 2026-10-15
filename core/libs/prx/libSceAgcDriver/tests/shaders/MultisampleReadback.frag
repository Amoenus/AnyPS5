#version 450

layout(push_constant) uniform Push { uint layer; uint width; uint height; } params;
#if !READ_DEPTH
layout(location = 0) out uvec4 color;
#endif

void main() {
    uint x = uint(gl_FragCoord.x);
    uint y = uint(gl_FragCoord.y);
#if READ_DEPTH
    gl_FragDepth = 0.125 + float(gl_SampleID) / 16.0 + float(params.layer) / 128.0 + float(x) / 1024.0 + float(y) / 16384.0;
#else
    uint value = x + y * params.width + params.layer * params.width * params.height + uint(gl_SampleID) * 31u;
    color = uvec4(0x91352700u ^ value, 0x45219b00u ^ (value * 3u), 0xc7391d00u ^ (value * 5u), 0x6b43e100u ^ (value * 7u));
#endif
}
