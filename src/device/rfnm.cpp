#ifdef BUILD_RFNM_SUPPORT
#include "rfnm.h"
#include "flog/flog.h"
#include <algorithm>
#include <chrono>
#include <cstring>

// RFNM backend via librfnm (>= 0.2.1). Bench rules encoded: stale channels recovered
// through the driver's canonical sweep at open, apply masks name only the configured
// channel, 20 s apply timeouts (a DCS reclock hides inside an apply), the analog
// filter tracks the sample rate via suggested_lpf_bw(). The stream format is CS16 -
// native for the local transport - converted to float at this edge; TX fills librfnm
// tx_buf packets in 256-sample multiples via tx_dqbuf/tx_qbuf after tx_work_start().

namespace dev {
    static constexpr uint32_t RFNM_APPLY_TIMEOUT_US = 20000000;
    // device-side minimum after the DCS-honesty work; rates synthesize in 1 kHz steps
    static constexpr double RFNM_MIN_SAMPLERATE = 196000.0;

    static bool parseIdent(const std::string& ident, enum rfnm::transport& transport, std::string& address) {
        transport = rfnm::TRANSPORT_FIND;
        address.clear();
        if (ident.empty() || ident == "find") { return true; }
        if (ident == "local") {
            transport = rfnm::TRANSPORT_LOCAL;
            return true;
        }
        size_t colon = ident.find(':');
        std::string head = ident.substr(0, (colon == std::string::npos) ? ident.size() : colon);
        std::string tail = (colon == std::string::npos) ? "" : ident.substr(colon+1);
        if (head == "usb") {
            transport = rfnm::TRANSPORT_USB;
            address = tail;
            return true;
        }
        if (head == "eth" || head == "tcp") {
            transport = rfnm::TRANSPORT_TCP;
            address = tail;
            return true;
        }
        if (colon == std::string::npos) {
            // a bare string is a USB serial
            transport = rfnm::TRANSPORT_USB;
            address = ident;
            return true;
        }
        return false;
    }

    // ================================= RECEIVER =================================

    RFNMReceiver::RFNMReceiver(RFNMDriver* drv, RFNMContext* ctx) {
        this->drv = drv;
        this->ctx = ctx;

        // Stage sane channel-0 defaults, kept RF-off (the stream start enables it):
        // preferred antenna path and a moderate gain, clamped by the device
        const rfnm_api_rx_ch* rx = ctx->dev->get_rx_channel(0);
        int8_t gain = (int8_t)std::min<int>(30, rx->gain_range.max);
        ctx->dev->set_rx_channel_status(0, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO, false);
        ctx->dev->set_rx_channel_path(0, rx->path_preferred, false);
        ctx->dev->set_rx_channel_gain(0, gain, false);
    }

    RFNMReceiver::~RFNMReceiver() {
        stop();
        close();
    }

    void RFNMReceiver::close() {
        if (ctx) { drv->releaseContext(ctx); }
        ctx = NULL;
    }

    double RFNMReceiver::getBestSamplerate(double min) {
        return std::max(ceil(min / 1e3) * 1e3, RFNM_MIN_SAMPLERATE);
    }

    void RFNMReceiver::setSamplerate(double samplerate) {
        drv->setDeviceSamplerate(ctx, samplerate);
        this->samplerate = samplerate;

        // the analog filter tracks the rate with the driver-suggested bandwidth
        int16_t bw = rfnm::device::suggested_lpf_bw((uint64_t)samplerate, &ctx->dev->get_hwinfo()->clock);
        ctx->dev->set_rx_channel_rfic_lpf_bw(0, bw, false);
        rfnm_api_failcode err = ctx->dev->apply(rfnm::rx_channel_apply_flags[0], true, RFNM_APPLY_TIMEOUT_US);
        if (err) { throw std::runtime_error(std::string("Failed to apply the RX samplerate: ") + rfnm::device::failcode_to_string(err)); }
    }

    void RFNMReceiver::tune(double freq) {
        ctx->dev->set_rx_channel_freq(0, (int64_t)freq, false);
        rfnm_api_failcode err = ctx->dev->apply(rfnm::rx_channel_apply_flags[0], true, RFNM_APPLY_TIMEOUT_US);
        if (err) { throw std::runtime_error(std::string("Failed to tune the RX channel: ") + rfnm::device::failcode_to_string(err)); }
    }

    void RFNMReceiver::start() {
        if (running) { return; }

        stream = std::unique_ptr<rfnm::rx_stream>(ctx->dev->rx_stream_create(rfnm::CH0));
        stream->set_auto_dc_offset(true, rfnm::CH0);
        rfnm_api_failcode err = stream->start();
        if (err) {
            stream.reset();
            throw std::runtime_error(std::string("Failed to start the RX stream: ") + rfnm::device::failcode_to_string(err));
        }

        // the stream start committed the rate to the clock chain; retighten the filter
        // if the actual ADC rate moved the suggestion
        if (ctx->dev->get(rfnm::REQ_HWINFO) == RFNM_API_OK) {
            int16_t bw = rfnm::device::suggested_lpf_bw((uint64_t)samplerate, &ctx->dev->get_hwinfo()->clock);
            if (bw != ctx->dev->get_rx_channel(0)->rfic_lpf_bw) {
                ctx->dev->set_rx_channel_rfic_lpf_bw(0, bw, false);
                ctx->dev->apply(rfnm::rx_channel_apply_flags[0], true, RFNM_APPLY_TIMEOUT_US);
            }
        }

        workerThread = std::thread(&RFNMReceiver::worker, this);
        running = true;
    }

    void RFNMReceiver::stop() {
        if (!running) { return; }

        out.stopWriter();
        if (workerThread.joinable()) { workerThread.join(); }
        out.clearWriteStop();

        stream->stop();
        stream.reset();
        running = false;
    }

    void RFNMReceiver::worker() {
        // ~200 blocks/s like the other backends, in CS16 straight off the transport
        int sampCount = std::min<int>(samplerate / 200.0, STREAM_BUFFER_SIZE);
        std::vector<int16_t> raw(2 * sampCount);
        void* buffs[1] = { raw.data() };
        constexpr float scale = 1.0f / 32768.0f;
        uint64_t lastDropped = 0;
        auto lastHealthPoll = std::chrono::steady_clock::now();

        while (true) {
            size_t got = 0;
            uint64_t ts = 0;
            rfnm_api_failcode err = stream->read(buffs, sampCount, got, ts, 100000);
            if (err != RFNM_API_OK && err != RFNM_API_TIMEOUT && err != RFNM_API_DQBUF_NO_DATA) {
                flog::error("RFNM RX stream error: {}", rfnm::device::failcode_to_string(err));
                break;
            }
            if (!got) { continue; } // timeouts are normal while the stream ramps up

            // CS16 -> float; -O3 auto-vectorizes this on NEON
            float* fout = (float*)out.writeBuf;
            for (size_t i = 0; i < 2 * got; i++) {
                fout[i] = (float)raw[i] * scale;
            }
            if (!out.swap(got)) { break; }

            // judge stream health by the transport counters, never by log lines.
            // get_health is a status round trip - poll at 1 Hz, not per block
            auto healthNow = std::chrono::steady_clock::now();
            if (healthNow - lastHealthPoll >= std::chrono::seconds(1)) {
                lastHealthPoll = healthNow;
                rfnm::health h = {};
                if (ctx->dev->get_health(&h) == RFNM_API_OK && h.rx_pkts_dropped > lastDropped) {
                    flog::warn("RFNM transport dropped {} packet(s); the sample stream has a gap", (int)(h.rx_pkts_dropped - lastDropped));
                    lastDropped = h.rx_pkts_dropped;
                }
            }
        }
    }

    // ================================ TRANSMITTER ================================

    RFNMTransmitter::RFNMTransmitter(RFNMDriver* drv, RFNMContext* ctx, dsp::stream<dsp::complex_t>* in) {
        this->drv = drv;
        this->ctx = ctx;
        this->in = in;

        if (!ctx->dev->get_tx_channel_count() || !ctx->dev->is_tx_channel_available(0)) {
            throw std::runtime_error("This RFNM device has no TX channel");
        }

        // stage sane TX defaults, kept RF-off until start
        const rfnm_api_tx_ch* tx = ctx->dev->get_tx_channel(0);
        ctx->dev->set_tx_channel_status(0, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO, false);
        ctx->dev->set_tx_channel_path(0, tx->path_preferred, false);
        ctx->dev->set_tx_channel_power(0, 0, false);
    }

    RFNMTransmitter::~RFNMTransmitter() {
        stop();
        close();
    }

    void RFNMTransmitter::close() {
        if (ctx) { drv->releaseContext(ctx); }
        ctx = NULL;
    }

    double RFNMTransmitter::getBestSamplerate(double min) {
        return std::max(ceil(min / 1e3) * 1e3, RFNM_MIN_SAMPLERATE);
    }

    void RFNMTransmitter::setSamplerate(double samplerate) {
        drv->setDeviceSamplerate(ctx, samplerate);
        rfnm_api_failcode err = ctx->dev->apply(rfnm::tx_channel_apply_flags[0], true, RFNM_APPLY_TIMEOUT_US);
        if (err) { throw std::runtime_error(std::string("Failed to apply the TX samplerate: ") + rfnm::device::failcode_to_string(err)); }
    }

    void RFNMTransmitter::tune(double freq) {
        ctx->dev->set_tx_channel_freq(0, (int64_t)freq, false);
        rfnm_api_failcode err = ctx->dev->apply(rfnm::tx_channel_apply_flags[0], true, RFNM_APPLY_TIMEOUT_US);
        if (err) { throw std::runtime_error(std::string("Failed to tune the TX channel: ") + rfnm::device::failcode_to_string(err)); }
    }

    void RFNMTransmitter::start() {
        if (running) { return; }

        // enable the channel and arm the librfnm TX workers (tx_qbuf without
        // tx_work_start queues buffers that nothing drains)
        ctx->dev->set_tx_channel_status(0, RFNM_CH_RF_ON, RFNM_CH_STREAM_AUTO, false);
        rfnm_api_failcode err = ctx->dev->apply(rfnm::tx_channel_apply_flags[0], true, RFNM_APPLY_TIMEOUT_US);
        if (err) { throw std::runtime_error(std::string("Failed to enable the TX channel: ") + rfnm::device::failcode_to_string(err)); }
        if (!ctx->txWorkStarted) {
            err = ctx->dev->tx_work_start();
            if (err) { throw std::runtime_error(std::string("Failed to start the TX workers: ") + rfnm::device::failcode_to_string(err)); }
            ctx->txWorkStarted = true;
        }

        workerThread = std::thread(&RFNMTransmitter::worker, this);
        running = true;
    }

    void RFNMTransmitter::stop() {
        if (!running) { return; }

        in->stopReader();
        if (workerThread.joinable()) { workerThread.join(); }
        in->clearReadStop();

        if (ctx->txWorkStarted) {
            ctx->dev->tx_work_stop();
            ctx->txWorkStarted = false;
        }
        ctx->dev->set_tx_channel_status(0, RFNM_CH_RF_OFF, RFNM_CH_STREAM_AUTO, false);
        ctx->dev->apply(rfnm::tx_channel_apply_flags[0], true, RFNM_APPLY_TIMEOUT_US);
        running = false;
    }

    void RFNMTransmitter::worker() {
        while (true) {
            int count = in->read();
            if (count <= 0) { break; }

            // float -> CS16 with saturation; -O3 auto-vectorizes this on NEON
            const float* fin = (const float*)in->readBuf;
            size_t base = txStage.size();
            txStage.resize(base + 2 * count);
            for (int i = 0; i < 2 * count; i++) {
                float v = fin[i] * 32767.0f;
                txStage[base + i] = (int16_t)std::clamp(v, -32768.0f, 32767.0f);
            }
            in->flush();

            // drain in 256-sample multiples (librfnm tx_buf granularity)
            size_t off = 0;
            while (txStage.size() - off >= 2 * 256) {
                size_t chunk = ((txStage.size() - off) / (2 * 256)) * 256;
                if (chunk > (size_t)RFNM_USB_TX_PACKET_ELEM_CNT) { chunk = RFNM_USB_TX_PACKET_ELEM_CNT; }
                rfnm::tx_buf* b = NULL;
                if (ctx->dev->tx_dqbuf(&b) != RFNM_API_OK || !b) {
                    flog::warn("RFNM TX queue saturated, dropping {} samples", (int)chunk);
                    off += 2 * chunk;
                    continue;
                }
                memcpy(b->buf, &txStage[off], chunk * 2 * sizeof(int16_t));
                b->elem_cnt = (uint32_t)chunk;
                b->tx_flags = 0;
                if (ctx->dev->tx_qbuf(b) != RFNM_API_OK) {
                    flog::error("RFNM tx_qbuf failed");
                    return;
                }
                off += 2 * chunk;
            }
            txStage.erase(txStage.begin(), txStage.begin() + off);
        }
    }

    // ================================== DRIVER ==================================

    void RFNMDriver::registerSelf() {
        registerDriver(RFNM_DRIVER_NAME, std::make_unique<RFNMDriver>());
    }

    std::vector<Info> RFNMDriver::list() {
        std::vector<Info> list;
        // NOTE: discovery broadcasts on the network and claims USB interfaces for its
        // duration - it only runs from the user-invoked --list command
        for (const auto& found : rfnm::device::find(rfnm::TRANSPORT_FIND)) {
            Info info;
            info.driver = RFNM_DRIVER_NAME;
            switch (found.transport) {
            case rfnm::TRANSPORT_LOCAL: info.identifier = "local"; break;
            case rfnm::TRANSPORT_USB:   info.identifier = "usb:" + found.address; break;
            case rfnm::TRANSPORT_TCP:   info.identifier = "eth:" + found.address; break;
            default:                    info.identifier = found.address; break;
            }
            bool hasTx = false;
            for (const auto& db : found.hwinfo.daughterboard) {
                if (db.tx_ch_cnt) { hasTx = true; }
            }
            info.type = hasTx ? (DEV_TYPE_RECEIVER | DEV_TYPE_TRANSMITTER) : DEV_TYPE_RECEIVER;
            list.push_back(info);
        }
        return list;
    }

    std::shared_ptr<Receiver> RFNMDriver::openRX(const std::string& identifier) {
        return std::make_shared<RFNMReceiver>(this, acquireContext(identifier));
    }

    std::shared_ptr<Transmitter> RFNMDriver::openTX(const std::string& identifier, dsp::stream<dsp::complex_t>* in) {
        return std::make_shared<RFNMTransmitter>(this, acquireContext(identifier), in);
    }

    RFNMContext* RFNMDriver::acquireContext(const std::string& identifier) {
        std::lock_guard<std::mutex> lck(mtx);

        // reuse an open context: the RX and TX halves of a full-duplex link share it
        auto it = contexts.find(identifier);
        if (it != contexts.end()) {
            it->second->refCount++;
            return it->second;
        }

        enum rfnm::transport transport;
        std::string address;
        if (!parseIdent(identifier, transport, address)) {
            throw std::runtime_error("Invalid RFNM device identifier: '" + identifier + "'");
        }

        RFNMContext* ctx = new RFNMContext;
        ctx->ident = identifier;
        ctx->refCount = 1;
        ctx->samplerate = 0;
        ctx->txWorkStarted = false;
        ctx->dev = new rfnm::device(transport, address);

        rfnm_api_failcode err = ctx->dev->get(rfnm::REQ_ALL);
        if (err) {
            delete ctx->dev;
            delete ctx;
            throw std::runtime_error(std::string("Failed to read the RFNM device status: ") + rfnm::device::failcode_to_string(err));
        }

        // recover channels a killed session left enabled - they block stream creation
        err = ctx->dev->rx_disable_stale_channels(RFNM_APPLY_TIMEOUT_US);
        if (err) {
            delete ctx->dev;
            delete ctx;
            throw std::runtime_error(std::string("Failed to disable stale RFNM channels: ") + rfnm::device::failcode_to_string(err));
        }

        // CS16 on the wire (native for the local transport), converted at this edge
        err = ctx->dev->set_stream_format(rfnm::STREAM_FORMAT_CS16);
        if (err) {
            delete ctx->dev;
            delete ctx;
            throw std::runtime_error(std::string("Failed to set the RFNM stream format: ") + rfnm::device::failcode_to_string(err));
        }

        contexts[identifier] = ctx;
        return ctx;
    }

    void RFNMDriver::releaseContext(RFNMContext* ctx) {
        std::lock_guard<std::mutex> lck(mtx);
        if (--ctx->refCount > 0) { return; }
        contexts.erase(ctx->ident);
        delete ctx->dev;
        delete ctx;
    }

    void RFNMDriver::setDeviceSamplerate(RFNMContext* ctx, double samplerate) {
        std::lock_guard<std::mutex> lck(mtx);

        // one DCS clock: the RX and TX rates are the same device-wide rate
        if (ctx->samplerate == samplerate) { return; }
        if (ctx->samplerate != 0 && ctx->samplerate != samplerate) {
            flog::warn("RFNM RX and TX share one clock: samplerate changed from {} to {}", ctx->samplerate, samplerate);
        }
        rfnm_api_failcode err = ctx->dev->set_samp_rate((uint64_t)samplerate, RFNM_APPLY_TIMEOUT_US);
        if (err) { throw std::runtime_error(std::string("Failed to set the RFNM samplerate: ") + rfnm::device::failcode_to_string(err)); }
        ctx->samplerate = samplerate;
    }
}

#endif
