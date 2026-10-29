#pragma once

#include "providers/contracts.h"
#include <QUrl>

namespace introvert::editor {

// Original-file access only. Resolve on a worker, then pass only the random
// loopback URL to Qt Multimedia. Authentication/upstream URLs stay private.
class RangeStream final {
public:
    static std::unique_ptr<RangeStream> open(const providers::ProviderConfig &config, const QString &episodeId,
                                              const providers::Cancellation &cancelled = {}, int resumeAttempts = 0);
    ~RangeStream();
    RangeStream(const RangeStream &) = delete;
    RangeStream &operator=(const RangeStream &) = delete;
    QUrl url() const;
    void close();
    int resumedRequests() const;
private:
    struct State;
    explicit RangeStream(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
};

} // namespace introvert::editor
