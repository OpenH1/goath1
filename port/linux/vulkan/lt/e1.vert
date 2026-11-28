// LT E1/E2's triangle over the window (the light-shadow passes' own: xgpu_spirv.c's XGPU_SPIRV_WORLD_AO_VERTEX)
#version 460

void main()
{
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(corner, 0.0, 1.0);
}
