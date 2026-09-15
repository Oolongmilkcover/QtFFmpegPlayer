#include "videowidget.h"
#include <QDebug>
#include <QTimer>
#include <QTextStream>
#include<sstream>
#include<iostream>
#include<QFile>
#include "perfclock.h"
#include <QElapsedTimer>
extern "C" {
#include <libavutil/frame.h>
}
//自动加双引号
#define GET_STR(x) #x
#define A_VER 3
#define T_VER 4

VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    //关闭垂直同步
    QSurfaceFormat fmt = format();
    fmt.setSwapInterval(0);
    setFormat(fmt);

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
    mux.lock();
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    mux.unlock();

    makeCurrent();

    glDeleteTextures(3, texs);

    doneCurrent();
}

void VideoWidget::clearScreen()
{
    mux.lock();
    //清掉缓存的那一帧 → paintGL 走"没帧"分支画黑
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    mux.unlock();
    update();    // 触发 paintGL，进入 !m_frame 分支 → 画黑
}

void VideoWidget::setFilterType(int type)
{
    if (type < 0 || type > 4) return;
    m_filterType.store(type);
    update();   // 触发重绘，shader 立刻应用新滤镜
}



//"上屏最新帧优先"：渲染线程调用，拿到 true 才有资格投递这一帧
bool VideoWidget::beginPaint()
{
    bool expected = false;
    return m_paintInFlight.compare_exchange_strong(expected, true);
}

void VideoWidget::endPaint()
{
    m_paintInFlight.store(false);
}

/*
 * 只"接管"这一帧的引用，不做任何拷贝：
 * 像素数据留在 AVFrame 里，paintGL() 直接从 frame->data[] 上传纹理。
 * 上一帧的引用在这里才还回去（所以 paintGL 期间数据一定有效）。
 */
void VideoWidget::setPaint(AVFrame *frame)
{
    if (!frame) return;

    mux.lock();
    //容错，保证尺寸正确
    if (width*height == 0 || frame->width != this->width
        || frame->height != this->height || !frame->data[0])
    {
        mux.unlock();
        av_frame_free(&frame);
        return;
    }
    if (m_frame) av_frame_free(&m_frame);   // 上一帧用完了
    m_frame = frame;                        // 零拷贝：只换指针
    mux.unlock();

    //刷新显示
    update();
}
void VideoWidget::Init(int width, int height)
{
    mux.lock();
    //尺寸可能会变，旧尺寸的帧直接丢掉
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    this->width = width;
    this->height = height;

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


    // ==========滤镜 uniform ==========
    m_filterLoc = program.uniformLocation("filterType");

    mux.unlock();
}

//刷新显示
void VideoWidget::paintGL()
{
    //=====统计真实屏幕帧率====
    static QElapsedTimer timer;
    if(!timer.isValid()) timer.start();
    m_realDrawCnt ++;
    qint64 ms = timer.elapsed();
    if(ms >= 1000)
    {
        double realFps = m_realDrawCnt * 1000.0 / ms;
        //qDebug() << "【屏幕真实渲染FPS】" << realFps;
        timer.restart();
        m_realDrawCnt = 0;
    }

    mux.lock();

    // 没有有效帧（还没 Init / 已 clearScreen / 尺寸不匹配）→ 画黑
    if (!m_frame || !m_frame->data[0] || width == 0 || height == 0) {
        mux.unlock();
        // 清屏为黑色
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    // =====================【needNsDiff完成打点】=====================
    // if(m_needNsDiff.load())
    // {
    //     m_needNsDiff.store(false);
    //     const qint64 nowNs  = perfNowNs();
    //     const qint64 costNs = nowNs - m_startNs;
    //     qDebug().nospace()
    //         << "[PERF] ===== FIRST VIDEO FRAME RENDERED cost = "
    //         << QString::number(costNs / 1e6, 'f', 2) << " ms";
    // }
    // =============================================================

    program.bind();

    //设置滤镜类型
    glUniform1i(m_filterLoc, m_filterType.load());

    /*
     * 直接从 AVFrame 的平面内存上传纹理（Y/U/V），
     * 不再经过 datas[] 中间缓冲 —— GUI 线程每帧少一次 3MB memcpy。
     * 注意：m_frame 在整个 paintGL 期间被 mux 保护着，数据一定有效。
     */
    AVFrame* f = m_frame;
    for (int p = 0; p < 3; ++p)
    {
        const int pw = (p == 0) ? width : width / 2;
        const int ph = (p == 0) ? height : height / 2;

        glActiveTexture(GL_TEXTURE0 + p);
        glBindTexture(GL_TEXTURE_2D, texs[p]); //第p层绑定到Y/U/V材质

        if (f->linesize[p] == pw)
        {
            //行宽正好等于平面宽度：一次上传（常见分辨率都是这种情况）
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph,
                            GL_RED, GL_UNSIGNED_BYTE, f->data[p]);
        }
        else
        {
            //有行对齐padding：逐行上传，源依然是解码器内存，没有中间拷贝
            for (int i = 0; i < ph; ++i)
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, pw, 1,
                                GL_RED, GL_UNSIGNED_BYTE,
                                f->data[p] + (size_t)f->linesize[p] * i);
            }
        }
        //与shader uni遍历关联
        glUniform1i(unis[p], p);
    }

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

// 测试用打点（默认关闭；需要时连同 videowidget.h 里的声明一起取消注释）
//void VideoWidget::setPerfStartNs(qint64 ns)
//{
//    m_startNs = ns;
//    //m_needNsDiff.store(false);
//    qDebug() << "[PERF] setPerfStartNs =" << ns <<" ns";
//}
