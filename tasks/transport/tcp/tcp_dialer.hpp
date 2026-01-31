#pragma once
#include <tasks/transport/dialer.hpp>
#include <tasks/transport/transport.hpp>
#include <string>

#include <tasks/transport/event_watcher/event_watcher.hpp>

using getrafty::io::EventWatcher;

class TcpDialer : public IDialer {
public:
    explicit TcpDialer(const std::string& address,
                       EventWatcher& ew);
    ~TcpDialer() override;

    void onConnected(F<void(std::unique_ptr<ITransport>)> cb) override { on_connected_ = std::move(cb); }
    void onFailure(F<void(int)> cb) override { on_failure_ = std::move(cb); }
    void dial() override;

private:
    void onWritable();

    std::string address_;
    EventWatcher& ew_;
    int fd_ = -1;

    F<void(std::unique_ptr<ITransport>)> on_connected_ = [](auto) {};
    F<void(int)> on_failure_ = [](int) {};
};
