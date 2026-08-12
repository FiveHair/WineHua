#version 450

layout(constant_id = 1) const uint variant = 0u;
layout(location = 0) out vec4 output_color;

void main()
{
    vec3 color = vec3(
        float((variant >> 0u) & 255u) / 255.0,
        float((variant >> 8u) & 255u) / 255.0,
        float((variant >> 16u) & 255u) / 255.0);
    output_color = vec4(color, 1.0);
}
