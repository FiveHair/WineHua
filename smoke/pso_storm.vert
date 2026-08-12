#version 450

layout(constant_id = 0) const float x_offset = 0.0;

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.5 + x_offset, 1.0);
}
