LIBRARY()

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

SRCS(
    public.cpp
    config.cpp
    job_report.cpp
    gpu_helpers.cpp
    gpu_info_provider.cpp
    helpers.cpp

    proto/supervisor_service.proto
)

PEERDIR(
    yt/yt/ytlib
    yt/yt/server/lib/job_agent
    yt/yt/server/lib/job_proxy
    yt/yt/server/lib/nbd
    yt/yt/library/containers
    yt/yt/library/containers/cri
    yt/yt/server/lib/misc
)

IF (NOT OPENSOURCE)
    SRCS(
        GLOBAL gpu_info_provider_impl.cpp
    )
    BUNDLE(
        yt/go/tar2squash
    )
    RESOURCE(
        tar2squash /tar2squash
    )
ENDIF()

END()

IF (NOT OPENSOURCE)
    RECURSE_FOR_TESTS(
        unittests
    )
ENDIF()
