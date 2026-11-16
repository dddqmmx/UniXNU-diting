// SPDX-License-Identifier: MIT
#version 450
layout(push_constant) uniform Params { vec4 color; vec4 extra; } pc;
layout(set=0, binding=0) uniform sampler2D image;
layout(location=0) out vec4 result;
void main() {
    const vec4 bars[8] = vec4[8](
        vec4(1,0,0,1), vec4(0,1,0,1), vec4(0,0,1,1), vec4(1,1,0,1),
        vec4(0,1,1,1), vec4(1,0,1,1), vec4(1,1,1,1), vec4(0,0,0,1));
    uint mode = uint(pc.extra.y);
    if (mode == 1) result = bars[(uint(gl_FragCoord.x) / 17 + uint(gl_FragCoord.y) / 11) & 7];
    else if (mode == 2) result = texture(image, gl_FragCoord.xy / pc.extra.zw);
    else if (mode == 3) result = vec4(gl_FragCoord.xy / pc.extra.zw, 0.25, 1);
    else result = pc.color;
}
