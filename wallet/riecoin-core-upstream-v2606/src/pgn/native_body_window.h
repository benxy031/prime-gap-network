#ifndef PGN_NATIVE_BODY_WINDOW_H
#define PGN_NATIVE_BODY_WINDOW_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <cassert>

namespace pgn {

enum class BodyArrival { Unrelated, Head, OutOfOrder };

template <class Hash>
class NativeBodyWindow
{
public:
    using Time = std::chrono::microseconds;

    // Existing receive and in-flight limits still apply. This only adjusts
    // how much of that capacity is worth committing to the next request.
    static std::size_t SuggestedWidth(std::size_t available, Time first_wait, Time body_service)
    {
        if (available <= 1 || first_wait <= body_service) return std::min<std::size_t>(1, available);
        const auto service = std::max(body_service, Time{1});
        const auto extra = static_cast<std::size_t>(first_wait / service);
        return extra >= available - 1 ? available : 1 + extra;
    }

    // Once the peer has answered the first item of a batch, bound silence
    // between bodies by its observed first response and validation cost.
    // Single-body downloads and an outstanding first body keep the old limit.
    static Time SuffixIdleTimeout(Time first_wait, Time body_service)
    {
        constexpr Time minimum_sample = std::chrono::seconds{2};
        constexpr Time maximum_sample = std::chrono::seconds{15};
        return 4 * std::clamp(std::max(first_wait, body_service), minimum_sample, maximum_sample);
    }

    bool Empty() const { return m_hashes.empty(); }
    bool Speculative() const { return m_speculative; }
    const Hash& Head() const { return m_hashes.front(); }
    std::size_t Count() const { return m_hashes.size(); }

    void Begin(const Hash& first)
    {
        m_speculative = false;
        m_hashes.clear();
        m_hashes.push_back(first);
    }

    void Append(const Hash& next)
    {
        m_hashes.push_back(next);
        m_speculative = true;
    }

    BodyArrival Classify(const Hash& received) const
    {
        if (Empty()) return BodyArrival::Unrelated;
        if (Head() == received) return BodyArrival::Head;
        return std::find(m_hashes.begin(), m_hashes.end(), received) == m_hashes.end()
            ? BodyArrival::Unrelated : BodyArrival::OutOfOrder;
    }

    // Call only after Core accepts the exact head on the required branch.
    bool Advance(const Hash& validated)
    {
        if (Classify(validated) != BodyArrival::Head) return false;
        m_hashes.pop_front();
        if (Empty()) m_speculative = false;
        return true;
    }

    void Reset()
    {
        m_hashes.clear();
        m_speculative = false;
    }

private:
    std::deque<Hash> m_hashes;
    bool m_speculative{false};
};

} // namespace pgn

#endif // PGN_NATIVE_BODY_WINDOW_H
