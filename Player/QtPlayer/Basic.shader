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
uniform int filterType;
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

    // 滤镜处理
    if (filterType == 1) {
        // 灰度：加权亮度（人眼感知权重）
        float gray = dot(rgb, vec3(0.299, 0.587, 0.114));
        rgb = vec3(gray);
    }
    else if (filterType == 2) {
        // 反色
        rgb = 1.0 - rgb;
    }
    else if (filterType == 3) {
        // 暖色：提红压蓝
        rgb = rgb * vec3(1.08, 0.96, 0.85);
    }
    else if (filterType == 4) {
        // 冷色：提蓝压红
        rgb = rgb * vec3(0.85, 0.96, 1.08);
    }

    // filterType == 0：原色，什么都不做
    color = vec4(rgb, 1.0);
}
