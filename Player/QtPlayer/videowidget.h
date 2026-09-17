#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <mutex>
#include<string>
#include <atomic>
struct AVFrame;
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    virtual void Init(int width, int height);

    //不管成功与否都释放frame空间
    virtual void setPaint(AVFrame *frame);

    /*
     * "上屏最新帧优先"用的两个钩子（渲染线程调用）：
     * 同一时刻只允许一帧在 GUI 线程里排队等上屏，GUI 线程忙不过来时
     * 渲染线程直接丢帧，而不是把带 buffer 的帧一层层堆在事件队列里。
     */
    bool beginPaint();
    void endPaint();

    VideoWidget(QWidget *parent);
    ~VideoWidget();

    // 清屏为黑色：释放帧数据并触发重绘
    void clearScreen();

    //滤镜：0原色 1灰度 2反色 3暖色 4冷色
    void setFilterType(int type);

    // 渲染线程叫这个，只存最新帧，不直接上屏
    //void submitFrame(AVFrame* frame);

protected:
    //刷新显示
    void paintGL();

    //初始化gl
    void initializeGL();

    // 窗口尺寸变化
    void resizeGL(int width, int height);
private:
    std::mutex mux;

    /*
     * 创建 Y/U/V 三张纹理（调用者必须持有 mux 且上下文已 current）。
     * Init() 和 initializeGL() 都会用到：前者是正常换台路径，
     * 后者用于"Init 时上下文还没就绪"的补偿。
     */
    void createTextures();

    //Init() 时上下文没就绪 → 纹理没建成，等 initializeGL() 里补
    bool m_needTextures = false;

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

    /*
     * 当前要显示的那一帧（持有引用）。
     * paintGL() 直接从这一帧的内存上传纹理，不再先 memcpy 到中间缓冲，
     * 所以 setPaint() 只是"换指针 + update()"，GUI 线程每帧少拷 3MB。
     */
    AVFrame* m_frame = nullptr;

    //是否已经有一帧正在 GUI 线程排队等上屏
    std::atomic<bool> m_paintInFlight{false};

    int width = 240;
    int height = 128;


    //顶点shader
    std::string vertexString;

    //片元shader
    std::string fragmentString ;

    qint64 m_realDrawCnt = 0;
    qint64 m_realFpsTimer = 0;



//--------------------------------------------------
    //这里几个都是测试用的（默认关闭，需要时取消注释）
    //qint64        m_startNs = 0;
//public:
    //std::atomic<bool> m_needNsDiff{false};

    //void setPerfStartNs(qint64 ns);
//--------------------------------------------------
};
