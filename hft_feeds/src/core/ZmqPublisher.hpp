#pragma once
#include <zmq.hpp>
#include <string>

class ZmqPublisher {
public:
    explicit ZmqPublisher(const std::string& bind_addr)
        : ctx_(1), pub_(ctx_, zmq::socket_type::pub)
    {
        // High-water mark: drop if subscriber is slow
        pub_.set(zmq::sockopt::sndhwm, 10000);
        pub_.bind(bind_addr);
    }

    void publish(const std::string& topic,
                 const std::string& payload)
    {
        zmq::message_t t(topic.data(), topic.size());
        zmq::message_t p(payload.data(), payload.size());

        pub_.send(t, zmq::send_flags::sndmore);
        pub_.send(p, zmq::send_flags::dontwait);
    }

private:
    zmq::context_t ctx_;
    zmq::socket_t  pub_;
};
