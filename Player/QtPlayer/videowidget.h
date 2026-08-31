#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <mutex>
#include<string>
struct AVFrame;
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    virtual void Init(int width, int height);

    //不管成功与否都释放frame空间
    virtual void setPaint(AVFrame *frame);

    VideoWidget(QWidget *parent);
    ~VideoWidget();

    // 清屏为黑色：释放帧数据并触发重绘
    void clearScreen();

    //滤镜：0原色 1灰度 2反色 3暖色 4冷色
    void setFilterType(int type);

protected:
    //刷新显示
    void paintGL();

    //初始化gl
    void initializeGL();

    // 窗口尺寸变化
    void resizeGL(int width, int height);
private:
    std::mutex mux;

    //shader程序
    QOpenGLShaderProgram program;

    //shader中yuv变量地址
    GLuint unis[3] = { 0 };
    //opengl的 texture地址
    GLuint texs[3] = { 0 };

    // ========== 滤镜 uniform 位置 ==========
    GLuint m_filterLoc = 0;
    // ========== 当前滤镜类型（原子，避免跨线程问题） ==========
    std::atomic<int> m_filterType{0};

    //材质内存空间
    unsigned char *datas[3] = { 0 };

    int width = 240;
    int height = 128;


    //顶点shader
    std::string vertexString;

    //片元shader
    std::string fragmentString ;

};
