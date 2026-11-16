// SPDX-License-Identifier: MIT
#version 450
layout(input_attachment_index=0,set=0,binding=0) uniform subpassInput prior;
layout(location=0) out vec4 result;
void main() { result = subpassLoad(prior); }
