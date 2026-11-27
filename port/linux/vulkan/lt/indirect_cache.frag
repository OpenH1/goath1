// The radiance cache's scatter's points (lt/indirect_cache.vert): their value, added.
#version 460

layout(location = 0) in vec4 value;
layout(location = 0) out vec4 result;

void main()
{
    result = value;
}
