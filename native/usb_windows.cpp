#include <Windows.h>
#include "CyAPI.h"
#include "engine.hpp"
#include <sstream>
#include <stdexcept>

namespace fx3 {
namespace {
std::string error_text(const char* operation, CCyUSBEndPoint* ep) {
    std::ostringstream out;
    out << operation << " failed: Win32=" << GetLastError()
        << ", LastError=" << ep->LastError << ", NTSTATUS=0x" << std::hex
        << ep->NtStatus << ", USBD_STATUS=0x" << ep->UsbdStatus;
    return out.str();
}
std::string utf8(const wchar_t* text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (!n) return {};
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), n, nullptr, nullptr);
    result.pop_back(); return result;
}
class UsbTransport final : public Transport {
    struct Slot {
        OVERLAPPED ov{};
        PUCHAR context = nullptr;
        uint8_t* data = nullptr;
        LONG size = 0;
        bool submitted = false;
        ~Slot() { if (ov.hEvent) CloseHandle(ov.hEvent); }
    };
    CCyUSBDevice device_{nullptr, CYUSBDRV_GUID, FALSE};
    CCyUSBEndPoint* endpoint_ = nullptr;
    std::vector<std::unique_ptr<Slot>> slots_;
public:
    UsbTransport(unsigned index, unsigned endpoint) {
        if (index >= device_.DeviceCount() || !device_.Open(static_cast<UCHAR>(index)))
            throw std::runtime_error("Cannot open CyUSB3 device; check driver, device index and interface GUID");
        endpoint_ = device_.EndPointOf(static_cast<UCHAR>(endpoint));
        if (!endpoint_ || endpoint_->Attributes != 2 || !endpoint_->bIn || !endpoint_->MaxPktSize)
            throw std::invalid_argument("The selected address is not an available Bulk IN endpoint");
        // CyAPI still allocates a small request context; payload goes directly into our pool.
        endpoint_->XferMode = XMODE_DIRECT;
    }
    ~UsbTransport() override { drain(); }
    size_t max_packet_size() const override { return endpoint_->MaxPktSize; }
    void prepare(const Config& config) override {
        drain();
        endpoint_->SetXferSize(static_cast<ULONG>(config.block_size));
        endpoint_->TimeOut = config.timeout_ms;
        for (size_t i = 0; i < config.queue_depth; ++i) {
            auto s = std::make_unique<Slot>();
            s->ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            if (!s->ov.hEvent) throw std::runtime_error("CreateEvent failed");
            slots_.push_back(std::move(s));
        }
    }
    void submit(size_t i, uint8_t* data, size_t size) override {
        auto& s = *slots_.at(i);
        if (s.context) throw std::logic_error("USB slot is still in flight");
        const auto event = s.ov.hEvent;
        s.ov = {}; s.ov.hEvent = event; ResetEvent(event);
        s.data = data; s.size = static_cast<LONG>(size);
        SetLastError(ERROR_SUCCESS); // CyAPI reads GetLastError even on synchronous success.
        s.context = endpoint_->BeginDataXfer(s.data, s.size, &s.ov);
        const auto error = endpoint_->LastError;
        if (!s.context) throw std::runtime_error("BeginDataXfer returned a null context");
        s.submitted = (error == ERROR_SUCCESS || error == ERROR_IO_PENDING);
        if (!s.submitted) {
            auto message = error_text("BeginDataXfer", endpoint_);
            LONG length = s.size;
            endpoint_->FinishDataXfer(s.data, length, &s.ov, s.context);
            s.context = nullptr;
            throw std::runtime_error(message);
        }
    }
    bool wait(size_t i, unsigned poll_ms) override {
        auto& s = *slots_.at(i);
        DWORD bytes = 0;
        if (GetOverlappedResult(device_.DeviceHandle(), &s.ov, &bytes, FALSE)) return true;
        if (GetLastError() != ERROR_IO_INCOMPLETE) return true; // finish reports a terminal error
        // LastError belongs to the endpoint, not each request. Normalize it for
        // this pending slot rather than depending on the last submitted request.
        endpoint_->LastError = ERROR_IO_PENDING;
        endpoint_->WaitForXfer(&s.ov, poll_ms);
        if (GetOverlappedResult(device_.DeviceHandle(), &s.ov, &bytes, FALSE)) return true;
        return GetLastError() != ERROR_IO_INCOMPLETE;
    }
    size_t finish(size_t i) override {
        auto& s = *slots_.at(i);
        LONG length = s.size;
        bool ok = endpoint_->FinishDataXfer(s.data, length, &s.ov, s.context);
        s.context = nullptr; s.submitted = false;
        if (!ok) throw std::runtime_error(error_text("FinishDataXfer", endpoint_));
        if (length < 0 || length > s.size) throw std::runtime_error("Invalid USB completion length");
        return static_cast<size_t>(length);
    }
    void drain() noexcept override {
        // Cancel every request before waiting on any one of them. Never release
        // OVERLAPPED, context or payload while the driver may still access it.
        for (auto& s : slots_)
            if (s->context && s->submitted) CancelIoEx(device_.DeviceHandle(), &s->ov);
        for (auto& s : slots_) {
            if (!s->context) continue;
            DWORD bytes = 0;
            if (s->submitted) GetOverlappedResult(device_.DeviceHandle(), &s->ov, &bytes, TRUE);
            LONG length = s->size;
            endpoint_->FinishDataXfer(s->data, length, &s->ov, s->context);
            s->context = nullptr; s->submitted = false;
        }
        slots_.clear();
    }
};
}
std::unique_ptr<Transport> open_usb(unsigned index, unsigned endpoint) {
    try { return std::make_unique<UsbTransport>(index, endpoint); }
    catch (const char* e) { throw std::runtime_error(e); }
}
std::vector<DeviceInfo> list_devices() {
    try {
        CCyUSBDevice device(nullptr, CYUSBDRV_GUID, FALSE);
        const unsigned count = device.DeviceCount();
        std::vector<DeviceInfo> result;
        for (unsigned i = 0; i < count; ++i) {
            if (device.Open(static_cast<UCHAR>(i))) {
                result.push_back({i, device.VendorID, device.ProductID, device.DeviceName, utf8(device.SerialNumber)});
                device.Close();
            }
        }
        return result;
    } catch (const char* e) { throw std::runtime_error(e); }
}
}
