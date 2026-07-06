#pragma once
#ifdef BUILD_RFNM_SUPPORT
#include "../device.h"
#include <librfnm/device.h>
#include <librfnm/rx_stream.h>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define RFNM_DRIVER_NAME "rfnm"

namespace dev {
    class RFNMDriver;

    // One physical device shared by the RX and TX halves: RyFi opens the RX and TX
    // sides separately, but on an RFNM they are usually the same full-duplex board.
    // The sample rate is device-wide (one DCS clock), so it lives here.
    struct RFNMContext {
        rfnm::device* dev;
        std::string ident;
        int refCount;
        double samplerate;
        bool txWorkStarted;
    };

    class RFNMReceiver : public Receiver {
    public:
        RFNMReceiver(RFNMDriver* drv, RFNMContext* ctx);
        ~RFNMReceiver();

        void close();
        double getBestSamplerate(double min);
        void setSamplerate(double samplerate);
        void tune(double freq);
        void start();
        void stop();

    private:
        void worker();

        RFNMDriver* drv;
        RFNMContext* ctx;
        std::unique_ptr<rfnm::rx_stream> stream;
        std::thread workerThread;
        double samplerate = 0;
    };

    class RFNMTransmitter : public Transmitter {
    public:
        RFNMTransmitter(RFNMDriver* drv, RFNMContext* ctx, dsp::stream<dsp::complex_t>* in);
        ~RFNMTransmitter();

        void close();
        double getBestSamplerate(double min);
        void setSamplerate(double samplerate);
        void tune(double freq);
        void start();
        void stop();

    private:
        void worker();

        RFNMDriver* drv;
        RFNMContext* ctx;
        std::thread workerThread;
        std::vector<int16_t> txStage;
    };

    class RFNMDriver : public Driver {
    public:
        static void registerSelf();

        std::vector<Info> list();
        std::shared_ptr<Receiver> openRX(const std::string& identifier);
        std::shared_ptr<Transmitter> openTX(const std::string& identifier, dsp::stream<dsp::complex_t>* in);

        RFNMContext* acquireContext(const std::string& identifier);
        void releaseContext(RFNMContext* ctx);
        void setDeviceSamplerate(RFNMContext* ctx, double samplerate);

    private:
        std::map<std::string, RFNMContext*> contexts;
        std::mutex mtx;
    };
}

#endif
