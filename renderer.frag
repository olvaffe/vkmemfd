#version 460 core

layout(std140, set = 0, binding = 0) uniform block {
    uniform vec4 color;
};

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = color;
}
