LIBRARY()

OWNER(
    spuchin
    g:kikimr
)

SRCS(
    kqp_opt_phy_build_stage.cpp
    kqp_opt_phy_limit.cpp
    kqp_opt_phy_olap_filter.cpp
    kqp_opt_phy_sort.cpp
    kqp_opt_phy_helpers.cpp
    kqp_opt_phy_stage_float_up.cpp
    kqp_opt_phy.cpp 
)

PEERDIR(
    ydb/core/kqp/common
    ydb/core/kqp/opt/physical/effects 
    ydb/library/yql/dq/common
    ydb/library/yql/dq/opt
)

YQL_LAST_ABI_VERSION()

END()
