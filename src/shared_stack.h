#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace wperf {
enum class StackQuality { Missing, Exact, Approximate };
class SharedStack {
public:
    SharedStack() = default;
    SharedStack(std::initializer_list<uint64_t> frames) : data_(std::make_shared<std::vector<uint64_t>>(frames)) {}
    SharedStack& operator=(std::initializer_list<uint64_t> frames) { data_=std::make_shared<std::vector<uint64_t>>(frames); return *this; }
    SharedStack& operator=(const std::vector<uint64_t>& frames) { data_=std::make_shared<std::vector<uint64_t>>(frames); return *this; }
    void Share(std::shared_ptr<std::vector<uint64_t>> frames) { data_=std::move(frames); }
    bool empty() const { return !data_ || data_->empty(); }
    size_t size() const { return data_ ? data_->size() : 0; }
    uint64_t front() const { return View().front(); }
    uint64_t operator[](size_t index) const { return View()[index]; }
    auto begin() const { return View().begin(); }
    auto end() const { return View().end(); }
    const std::vector<uint64_t>& View() const { static const std::vector<uint64_t> empty; return data_ ? *data_ : empty; }
    operator std::vector<uint64_t>() const { return View(); }
private:
    std::shared_ptr<std::vector<uint64_t>> data_;
};

}  // namespace wperf
