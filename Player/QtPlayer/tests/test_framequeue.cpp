/*
 * FrameQueue 逐帧状态机单元测试
 *
 * 特点：只依赖 Qt6::Core + FFmpeg(avutil)，**不创建任何窗口 / QApplication**，
 *       所以可以在无桌面环境直接跑（CI、无桌面虚拟机）：
 *           QT_QPA_PLATFORM=offscreen ctest --output-on-failure
 *
 * 覆盖：前进 / 后退 / 历史耗尽触发回填 / 回填后无缝接续 / 到文件开头 /
 *       回填失败不再重复请求 / 历史容量与淘汰 / 清空。
 */
#include "framequeue.h"

#include <cstdio>
#include <cassert>
#include <deque>
#include <vector>

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  [FAIL] %s (line %d)\n", #cond, __LINE__);                \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

// 造一个带真实 buffer 的 AVFrame（这样 clone / unref 的引用计数路径也被覆盖）
static AVFrame* makeFrame(int64_t pts)
{
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_GRAY8;
    f->width  = 4;
    f->height = 4;
    av_frame_get_buffer(f, 0);
    f->pts = pts;
    return f;
}

// 生产者：往未来队列放一帧（看护好 pts 必须是毫秒，与生产代码一致）
static void produce(FrameQueue& q, int64_t pts, int serial = 1)
{
    Frame* w = q.getWritable();
    assert(w);
    AVFrame* f = makeFrame(pts);
    av_frame_unref(w->m_frame);
    av_frame_move_ref(w->m_frame, f);
    av_frame_free(&f);
    w->m_frame->pts = pts;
    w->m_serial = serial;
    q.push();
}

// 消费者：正常播放一帧（该帧进入历史）
static void playOne(FrameQueue& q)
{
    Frame* r = q.getReadable();
    assert(r);
    q.next();
}

static int64_t stepFwd(FrameQueue& q)
{
    Frame* f = q.stepForward();
    if (!f) return -1;
    int64_t pts = f->m_frame ? f->m_frame->pts : -1;
    delete f;
    return pts;
}

static int64_t stepBack(FrameQueue& q, FrameQueue::StepStatus* st)
{
    Frame* f = q.stepBackward(st);
    if (!f) return -1;
    int64_t pts = f->m_frame ? f->m_frame->pts : -1;
    delete f;
    return pts;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ==================== 1. 前进 / 后退基本逻辑 ====================
    {
        printf("== 1. stepForward / stepBackward\n");
        FrameQueue q(16, true);
        long long refillPts = -1;
        int refillCnt = 0;
        q.onNeedRefill = [&](long long pts) { refillPts = pts; ++refillCnt; };

        for (int i = 0; i < 16; ++i) produce(q, 1000 + i * 40);
        CHECK(q.size() == 16);

        // 播放 5 帧：1000,1040,1080,1120,1160
        for (int i = 0; i < 5; ++i) playOne(q);
        CHECK(q.historySize() == 5);
        CHECK(q.currentPts() == 1160);
        CHECK(q.cursorAtNewest());
        CHECK(q.size() == 11);

        FrameQueue::StepStatus st = FrameQueue::StepEmpty;
        CHECK(stepBack(q, &st) == 1120); CHECK(st == FrameQueue::StepOk);
        CHECK(stepBack(q, &st) == 1080); CHECK(st == FrameQueue::StepOk);
        CHECK(q.cursorAtNewest() == false);
        CHECK(q.currentPts() == 1080);

        // 前进两帧回到最新
        CHECK(stepFwd(q) == 1120);
        CHECK(stepFwd(q) == 1160);
        CHECK(q.cursorAtNewest());

        // 从未来队列继续前进
        CHECK(stepFwd(q) == 1200);
        CHECK(q.size() == 10);
        CHECK(q.historySize() == 6);
        CHECK(q.currentPts() == 1200);

        // 一路退到历史最旧（1000）
        int64_t expect = 1160;
        while (expect >= 1000) {
            CHECK(stepBack(q, &st) == expect);
            expect -= 40;
        }
        CHECK(q.currentPts() == 1000);

        // 光标已经停在最旧帧，再按一次才会请求回填，边界 pts = 1000
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepNeedRefill);
        CHECK(refillCnt == 1);
        CHECK(refillPts == 1000);

        // 回填还没完成 → 再按一次不应该重复请求
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepNeedRefill);
        CHECK(refillCnt == 1);

        // 回填还没结束时前进/后退
        CHECK(stepFwd(q) == 1040);
        CHECK(stepBack(q, &st) == 1000);
    }

    // ==================== 2. 回填历史 ====================
    {
        printf("== 2. prependHistory\n");
        FrameQueue q(16, true);
        long long refillPts = -1;
        int refillCnt = 0;
        q.onNeedRefill = [&](long long pts) { refillPts = pts; ++refillCnt; };

        for (int i = 0; i < 16; ++i) produce(q, 1000 + i * 40);
        for (int i = 0; i < 5; ++i) playOne(q);          // 1000..1160

        FrameQueue::StepStatus st;
        while (stepBack(q, &st) != -1) { }               // 退到最旧并触发回填
        CHECK(refillCnt == 1 && refillPts == 1000);

        // 模拟 demux 回填：更早的 6 帧 760..960
        std::deque<AVFrame*> batch;
        for (int i = 0; i < 6; ++i) batch.push_back(makeFrame(760 + i * 40));
        q.prependHistory(batch, 7);
        CHECK(batch.empty());
        CHECK(q.historySize() == 11);
        CHECK(q.historyOldestPts() == 760);
        // 光标仍停在原来那一帧（1000）
        CHECK(q.currentPts() == 1000);
        CHECK(q.cursorAtNewest() == false);

        // 继续后退
        int64_t expect = 960;
        for (int i = 0; i < 6; ++i) {
            CHECK(stepBack(q, &st) == expect);
            expect -= 40;
        }
        CHECK(q.currentPts() == 760);
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepNeedRefill);
        CHECK(refillCnt == 2 && refillPts == 760);

        // 回退后再前进，能回到边界 1000
        expect = 800;
        for (int i = 0; i < 5; ++i) {
            CHECK(stepFwd(q) == expect);
            expect += 40;
        }
        CHECK(stepFwd(q) == 1000);
        CHECK(stepFwd(q) == 1040);      // 历史里的
        CHECK(stepFwd(q) == 1080);
        CHECK(stepFwd(q) == 1120);
        CHECK(stepFwd(q) == 1160);
        CHECK(q.cursorAtNewest());
        CHECK(stepFwd(q) == 1200);      // 未来队列里的
    }

    // ==================== 3. 到文件开头 ====================
    {
        printf("== 3. at begin\n");
        FrameQueue q(16, true);
        int refillCnt = 0;
        q.onNeedRefill = [&](long long) { ++refillCnt; };

        for (int i = 0; i < 16; ++i) produce(q, i * 40);   // 第一帧 pts = 0
        for (int i = 0; i < 4; ++i) playOne(q);

        FrameQueue::StepStatus st;
        CHECK(stepBack(q, &st) == 80);
        CHECK(stepBack(q, &st) == 40);
        CHECK(stepBack(q, &st) == 0);
        CHECK(st == FrameQueue::StepOk);
        // 已经是第一帧 → 不再请求回填
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepAtBegin);
        CHECK(refillCnt == 0);
    }

    // ==================== 4. 回填失败（到文件开头） ====================
    {
        printf("== 4. refill fail -> at begin\n");
        FrameQueue q(16, true);
        int refillCnt = 0;
        q.onNeedRefill = [&](long long) { ++refillCnt; };

        for (int i = 0; i < 16; ++i) produce(q, 1000 + i * 40);
        for (int i = 0; i < 3; ++i) playOne(q);

        FrameQueue::StepStatus st;
        CHECK(stepBack(q, &st) == 1040);
        CHECK(stepBack(q, &st) == 1000);
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepNeedRefill);
        CHECK(refillCnt == 1);

        // demux 回填不到更早的帧
        q.finishRefill(false);
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepAtBegin);
        CHECK(refillCnt == 1);           // 不会无限请求

        // 往前走了新帧之后，"到开头"的结论作废，还能继续尝试回填
        CHECK(stepFwd(q) == 1040);
        CHECK(stepFwd(q) == 1080);
        CHECK(stepFwd(q) == 1120);       // 从未来队列取，历史边界后移
        CHECK(q.cursorAtNewest());
        CHECK(stepBack(q, &st) == 1080);
        CHECK(stepBack(q, &st) == 1040);
        CHECK(stepBack(q, &st) == 1000);
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepNeedRefill);
        CHECK(refillCnt == 2);
    }

    // ==================== 5. 历史容量与淘汰 ====================
    {
        printf("== 5. history capacity\n");
        FrameQueue q(16, true);
        for (int i = 0; i < 16; ++i) produce(q, i * 40);
        for (int i = 0; i < 16; ++i) playOne(q);
        CHECK(q.historySize() == 16);

        // 正常播放会一直裁剪最旧的，历史稳定在容量上
        for (int i = 0; i < 40; ++i) { produce(q, 1000 + i * 40); playOne(q); }
        CHECK(q.historySize() == FrameQueue::MAX_HISTORY_SIZE);
        CHECK(q.currentPts() == 1000 + 39 * 40);
        // 最旧的那一帧：56 帧里最后 48 帧的第一帧 = 第 8 帧 = pts 320
        CHECK(q.historyOldestPts() == 8 * 40);

        // 退到历史最旧帧，再回填一批更早的帧
        FrameQueue::StepStatus st;
        while (q.currentPts() != q.historyOldestPts()) { CHECK(stepBack(q, &st) != -1); }
        CHECK(q.cursorAtNewest() == false);

        std::deque<AVFrame*> batch;
        for (int i = 0; i < 40; ++i) batch.push_back(makeFrame(i * 10));
        q.prependHistory(batch, 3);
        CHECK(q.historySize() == FrameQueue::MAX_HISTORY_SIZE);
        CHECK(q.historyOldestPts() == 0);            // 入帧里最早的还在
        CHECK(q.currentPts() > 0);                   // 光标还停在原来那一帧

        // 继续后退，能退满整批回填帧
        int64_t cnt = 0, lastPts = q.currentPts();
        while (stepBack(q, &st) != -1) { lastPts = q.currentPts(); ++cnt; }
        CHECK(cnt == 40);
        CHECK(lastPts == 0);
    }

    // ==================== 6. 清空 ====================
    {
        printf("== 6. clear / clearFuture\n");
        FrameQueue q(16, true);
        for (int i = 0; i < 16; ++i) produce(q, i * 40);
        for (int i = 0; i < 5; ++i) playOne(q);

        q.clearFuture();
        CHECK(q.size() == 0);
        CHECK(q.historySize() == 5);      // 历史保留

        q.clear();
        CHECK(q.size() == 0);
        CHECK(q.historySize() == 0);
        CHECK(q.currentPts() == -1);

        FrameQueue::StepStatus st;
        CHECK(stepBack(q, &st) == -1);
        CHECK(st == FrameQueue::StepAtBegin);
    }

    if (g_fail == 0) { printf("\nALL TESTS PASSED\n"); return 0; }
    printf("\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
