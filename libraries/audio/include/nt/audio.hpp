#pragma once
#include <cstdint>
#include <memory>

namespace nt::audio {
// Native output ownership. No console register file or instruction interface.
class Output {
public:
    virtual ~Output()=default;
    virtual bool open()=0;
    virtual bool initialize(bool preserve)=0;
    virtual void close()=0;
    virtual bool ready() const=0;
    virtual unsigned voices() const=0;
    virtual std::uint32_t error() const=0;
};
std::unique_ptr<Output> make_output();
}
