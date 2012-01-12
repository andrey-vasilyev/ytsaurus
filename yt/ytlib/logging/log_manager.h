#pragma once

#include "common.h"

#include <ytlib/ytree/ytree_fwd.h>

namespace NYT {
namespace NLog {

////////////////////////////////////////////////////////////////////////////////

class TLogManager
{
public:
    TLogManager();

    static TLogManager* Get();

    void Configure(NYTree::INode* node);
    void Configure(const Stroka& fileName, const NYTree::TYPath& path);

    void Flush();
    void Shutdown();

    int GetConfigVersion();
    void GetLoggerConfig(
        const Stroka& category,
        ELogLevel* minLevel,
        int* configVersion);

    void Write(const TLogEvent& event);

private:
    class TImpl;
    THolder<TImpl> Impl;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT

template <>
struct TSingletonTraits<NYT::NLog::TLogManager> {
    enum {
        Priority = 2048
    };
};
