#include "VideoWidget.h"
#include <QDebug>
#include <QTimer>
#include <QTextStream>
#include<sstream>
#include<iostream>
#include<QFile>

extern "C" {
#include <libavutil/frame.h>
}
//自动加双引号
#define GET_STR(x) #x
#define A_VER 3
#define T_VER 4


//准备yuv数据
// ffmpeg -i v1080.mp4 -t 10 -s 240x128 -pix_fmt yuv420p  out240x128.yuv
VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    QFile file(":/Basic.shader");

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        std::cout << "无法打开 Shader 文件" << std::endl;
        return;
    }

    enum class ShaderType {
        NONE = -1,
        VERTEX = 0,
        FRAGMENT = 1
    };

    std::stringstream ss[2];

    ShaderType type = ShaderType::NONE;

    QTextStream stream(&file);

    QString line;

    while (!stream.atEnd())
    {
        line = stream.readLine();

        if (line.contains("#shader"))
        {
            if (line.contains("vertex"))
            {
                type = ShaderType::VERTEX;
            }
            else if (line.contains("fragment"))
            {
                type = ShaderType::FRAGMENT;
            }
        }
        else
        {
            if (type != ShaderType::NONE)
            {
                ss[(int)type] << line.toStdString() << '\n';
            }
        }
    }

    file.close();

    vertexString = ss[0].str();
    fragmentString = ss[1].str();

    // std::cout<<"vertexString:"<<vertexString<<std::endl;
    // std::cout<<"fragmentString:"<<fragmentString<<std::endl;
}

VideoWidget::~VideoWidget()
{
    delete[] datas[0];
    delete[] datas[1];
    delete[] datas[2];

    makeCurrent();

    glDeleteTextures(3, texs);

    doneCurrent();
}

void VideoWidget::clearScreen()
{
    mux.lock();
    delete[] datas[0];
    delete[] datas[1];
    delete[] datas[2];
    datas[0] = nullptr;
    datas[1] = nullptr;
    datas[2] = nullptr;
    mux.unlock();
    update();    // 触发 paintGL，进入 !datas[0] 分支 → 画黑
}



void VideoWidget::setPaint(AVFrame *frame)
{

    if (!frame)return;
    mux.lock();
    //容错，保证尺寸正确
    if (!datas[0] || width*height == 0 || frame->width != this->width || frame->height != this->height)
    {
        av_frame_free(&frame);
        mux.unlock();
        return;
    }
    if (width == frame->linesize[0]) //无需对齐
    {
        memcpy(datas[0], frame->data[0], width*height);
        memcpy(datas[1], frame->data[1], width*height / 4);
        memcpy(datas[2], frame->data[2], width*height / 4);
    }
    else//行对齐问题
    {
        for(int i = 0; i < height; i++) //Y
            memcpy(datas[0] + width*i, frame->data[0] + frame->linesize[0]*i, width);
        for (int i = 0; i < height/2; i++) //U
            memcpy(datas[1] + width/2*i, frame->data[1] + frame->linesize[1] * i, width/2);
        for (int i = 0; i < height/2; i++) //V
            memcpy(datas[2] + width/2*i, frame->data[2] + frame->linesize[2] * i, width/2);

    }

    mux.unlock();
    av_frame_free(&frame);
    //qDebug() << "刷新显示" << endl;
    //刷新显示
    update();
}
void VideoWidget::Init(int width, int height)
{
    mux.lock();
    this->width = width;
    this->height = height;

    delete[] datas[0];
    delete[] datas[1];
    delete[] datas[2];

    datas[0] = nullptr;
    datas[1] = nullptr;
    datas[2] = nullptr;

    ///分配材质内存空间
    datas[0] = new unsigned char[width*height];		//Y
    datas[1] = new unsigned char[width*height / 4];	//U
    datas[2] = new unsigned char[width*height / 4];	//V

    makeCurrent();
    if (texs[0])
    {
        glDeleteTextures(3, texs);
    }
    //创建材质
    glGenTextures(3, texs);

    //Y
    glBindTexture(GL_TEXTURE_2D, texs[0]);
    //放大过滤，线性插值   GL_NEAREST(效率高，但马赛克严重)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    //创建材质显卡空间
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    //U
    glBindTexture(GL_TEXTURE_2D, texs[1]);
    //放大过滤，线性插值
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    //创建材质显卡空间
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    //V
    glBindTexture(GL_TEXTURE_2D, texs[2]);
    //放大过滤，线性插值
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    //创建材质显卡空间
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    doneCurrent();
    mux.unlock();


}
//初始化opengl
void VideoWidget::initializeGL()
{
    qDebug() << "initializeGL";
    mux.lock();

    initializeOpenGLFunctions();

    // 编译 Fragment Shader
    if (!program.addShaderFromSourceCode(
            QOpenGLShader::Fragment,
            fragmentString.c_str()))
    {
        qDebug() << "Fragment Shader Error:";
        qDebug() << program.log();
    }

    // 编译 Vertex Shader
    if (!program.addShaderFromSourceCode(
            QOpenGLShader::Vertex,
            vertexString.c_str()))
    {
        qDebug() << "Vertex Shader Error:";
        qDebug() << program.log();
    }


    // 链接 Shader Program
    if (!program.link())
    {
        qDebug() << "Shader Link Error:";
        qDebug() << program.log();
    }

    // 使用 Shader Program
    if (!program.bind())
    {
        qDebug() << "Shader Bind Error:";
        qDebug() << program.log();
    }

    //传递顶点和材质坐标
    //顶点
    static const GLfloat ver[] = {
        -1.0f , -1.0f,
         1.0f , -1.0f,
        -1.0f ,  1.0f,
         1.0f ,  1.0f
    };

    //材质
    static const GLfloat tex[] = {
        0.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f
    };

    //顶点
    glVertexAttribPointer(A_VER, 2, GL_FLOAT, 0, 0, ver);
    glEnableVertexAttribArray(A_VER);

    //材质
    glVertexAttribPointer(T_VER, 2, GL_FLOAT, 0, 0, tex);
    glEnableVertexAttribArray(T_VER);


    //从shader获取材质
    unis[0] = program.uniformLocation("tex_y");
    unis[1] = program.uniformLocation("tex_u");
    unis[2] = program.uniformLocation("tex_v");




    mux.unlock();
}

//刷新显示
void VideoWidget::paintGL()
{
    mux.lock();

    //暂时做法 ，更健壮的做法：增加一个“有效帧”标志 来判断是否要黑屏
    // 检查是否有有效纹理数据（例如 datas[0] 是否已分配）
    if (!datas[0] || width == 0 || height == 0) {
        mux.unlock();
        // 清屏为黑色
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    program.bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texs[0]); //0层绑定到Y材质
    //修改材质内容(复制内存内容)
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, datas[0]);
    //与shader uni遍历关联
    glUniform1i(unis[0], 0);


    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, texs[1]); //1层绑定到U材质
    //修改材质内容(复制内存内容)
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_BYTE, datas[1]);
    //与shader uni遍历关联
    glUniform1i(unis[1], 1);


    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, texs[2]); //2层绑定到V材质
    //修改材质内容(复制内存内容)
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_BYTE, datas[2]);
    //与shader uni遍历关联
    glUniform1i(unis[2], 2);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // qDebug() << "paintGL";

    program.release();

    mux.unlock();
}


// 窗口尺寸变化
void VideoWidget::resizeGL(int width, int height)
{
    mux.lock();
    qDebug() << "resizeGL " << width << ":" << height;
    //glViewport(0,0,width,height);
    mux.unlock();
}
