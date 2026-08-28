#shader vertex
#version 330 core

layout(location = 3) in vec2 vertexIn;
layout(location = 4) in vec2 textureIn;

out vec2 textureOut;

void main()
{
    gl_Position = vec4(vertexIn, 0.0, 1.0);
    textureOut = textureIn;
}

#shader fragment
#version 330 core

in vec2 textureOut;

uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;

out vec4 color;

void main()
{
    vec3 yuv;
    vec3 rgb;

    yuv.x = texture(tex_y, textureOut).r;
    yuv.y = texture(tex_u, textureOut).r - 0.5;
    yuv.z = texture(tex_v, textureOut).r - 0.5;

    rgb = mat3(
        1.0,      1.0,      1.0,
        0.0,     -0.39465,  2.03211,
        1.13983, -0.58060,  0.0
    ) * yuv;

    color = vec4(rgb, 1.0);
}
