
#include "AudioThread.h"
#include "Decode.h"
#include "AudioPlay.h"
#include "Resample.h"
#include <iostream>
using namespace std;

void AudioThread::Clear()
{
    DecodeThread::Clear();
    mux.lock();
    if (ap) ap->Clear();
    mux.unlock();
}

void AudioThread::Close()
{
    DecodeThread::Close();
    if (res)
    {
        res->Close();
        amux.lock();
        delete res;
        res = NULL;
        amux.unlock();
    }
    if (ap)
    {
        ap->Close();
        amux.lock();
        ap = NULL;
        amux.unlock();
    }
}

bool AudioThread::Open(AVCodecParameters *para, int sampleRate, int channels)
{
    if (!para) return false;
    Clear();

    amux.lock();
    pts = 0;
    bool re = true;

    if (!res->Open(para, false))
    {
        cout << "Resample open failed!" << endl;
        re = false;
    }
    ap->sampleRate = sampleRate;
    ap->channels = channels;
    if (!ap->Open())
    {
        re = false;
        cout << "AudioPlay open failed!" << endl;
    }
    if (!decode->Open(para))
    {
        cout << "audio Decode open failed!" << endl;
        re = false;
    }
    amux.unlock();
    cout << "AudioThread::Open :" << re << endl;
    return re;
}

void AudioThread::SetPause(bool isPause)
{
    this->isPause = isPause;
    if (ap)
        ap->SetPause(isPause);
}

void AudioThread::run()
{
    unsigned char *pcm = new unsigned char[1024 * 1024 * 10];
    while (!isExit)
    {
        // 1：只在取包时加锁，减少锁的粒度
        AVPacket *pkt = Pop();
        if (!pkt)
        {
            msleep(1);
            continue;
        }

        // 2：解码和播放不在锁里，保证音频流畅
        bool re = decode->Send(pkt);
        if (!re)
        {
            continue;
        }

        // 一次send 多次recv
        while (!isExit)
        {
            AVFrame * frame = decode->Recv();
            if (!frame) break;

            // 减去缓冲中未播放的时间
            pts = decode->pts - ap->GetNoPlayMs();

            // 重采样
            int size = res->Resampledata(frame, pcm);

            // 3：直接播放，不做复杂等待
            if (size > 0 && !isPause)
            {
                // 只在缓冲区满时才等待
                while (!isExit && ap->GetFree() < size)
                {
                    msleep(1);
                }
                ap->Write(pcm, size);
            }
        }
    }
    delete[] pcm;
}

AudioThread::AudioThread()
{
    if (!res) res = new Resample();
    if (!ap) ap = AudioPlay::Get();
}

AudioThread::~AudioThread()
{
    isExit = true;
    wait();
}
