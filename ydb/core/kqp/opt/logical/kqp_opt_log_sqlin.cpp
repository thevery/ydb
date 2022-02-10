#include "kqp_opt_log_rules.h"

#include <ydb/core/kqp/opt/kqp_opt_impl.h>
#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/provider/yql_kikimr_provider_impl.h>
#include <ydb/core/kqp/provider/yql_kikimr_opt_utils.h>

#include <ydb/library/yql/core/common_opt/yql_co_sqlin.h>

namespace NKikimr::NKqp::NOpt { 

using namespace NYql;
using namespace NYql::NDq;
using namespace NYql::NNodes;

TExprBase KqpRewriteSqlInToEquiJoin(const TExprBase& node, TExprContext& ctx, const TKqpOptimizeContext& kqpCtx,
    const TKikimrConfiguration::TPtr& config)
{
    if (!kqpCtx.IsDataQuery()) {
        return node;
    }

    if (config->HasOptDisableSqlInToJoin()) {
        return node;
    }

    if (!node.Maybe<TCoFlatMap>()) {
        return node;
    }

    const auto flatMap = node.Cast<TCoFlatMap>();
    const auto lambdaBody = flatMap.Lambda().Body();

    // SqlIn expected to be rewritten to (FlatMap <in> (OptionalIf ...)) or (FlatMap <in> (FlatListIf ...))
    if (!lambdaBody.Maybe<TCoOptionalIf>() && !lambdaBody.Maybe<TCoFlatListIf>()) {
        return node;
    }

    if (!FindNode(lambdaBody.Ptr(), [](const TExprNode::TPtr& x) { return TCoSqlIn::Match(x.Get()); })) {
        return node;
    }

    if (!flatMap.Input().Maybe<TKqlReadTable>() && !flatMap.Input().Maybe<TKqlReadTableIndex>()) {
        return node;
    }

    const auto readTable = flatMap.Input().Cast<TKqlReadTableBase>();

    if (!readTable.Table().SysView().Value().empty()) {
        return node;
    }

    TString lookupTable;

    if (auto indexRead = flatMap.Input().Maybe<TKqlReadTableIndex>()) {
        lookupTable = GetIndexMetadata(indexRead.Cast(), *kqpCtx.Tables, kqpCtx.Cluster)->Name;
    } else {
        lookupTable = readTable.Table().Path().StringValue();
    }

    const auto& tableDesc = kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, lookupTable);
    const auto& rangeFrom = readTable.Range().From();
    const auto& rangeTo = readTable.Range().To();

    if (!rangeFrom.Maybe<TKqlKeyInc>() || !rangeTo.Maybe<TKqlKeyInc>()) {
        return node;
    }
    if (rangeFrom.Raw() != rangeTo.Raw()) {
        // not point selection
        return node;
    }

    i64 keySuffixLen = (i64) tableDesc.Metadata->KeyColumnNames.size() - (i64) rangeFrom.ArgCount();
    if (keySuffixLen <= 0) {
        return node;
    }

    TVector<TStringBuf> keys; // remaining key parts, that can be used in SqlIn (only in asc order)
    keys.reserve(keySuffixLen);
    for (ui64 idx = rangeFrom.ArgCount(); idx < tableDesc.Metadata->KeyColumnNames.size(); ++idx) {
        keys.emplace_back(TStringBuf(tableDesc.Metadata->KeyColumnNames[idx]));
    }

    auto flatMapLambdaArg = flatMap.Lambda().Args().Arg(0);

    auto findMemberIndexInKeys = [&keys](const TCoArgument& flatMapLambdaArg, const TCoMember& member) {
        if (member.Struct().Raw() != flatMapLambdaArg.Raw()) {
            return -1;
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            if (member.Name().Value() == keys[i]) {
                return (int) i;
            }
        }
        return -1;
    };

    auto shouldConvertSqlInToJoin = [&](const TCoSqlIn& sqlIn, bool negated) {
        if (negated) {
            // negated can't be rewritten to the index-lookup, so skip it
            return false;
        }

        // validate key prefix
        if (sqlIn.Lookup().Maybe<TCoMember>()) {
            if (findMemberIndexInKeys(flatMapLambdaArg, sqlIn.Lookup().Cast<TCoMember>()) != 0) {
                return false;
            }
        } else if (sqlIn.Lookup().Ref().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Tuple) {
            auto children = sqlIn.Lookup().Ref().ChildrenList();
            TVector<int> usedKeyIndexes{Reserve(children.size())};
            for (const auto& itemPtr : children) {
                TExprBase item{itemPtr};
                if (!item.Maybe<TCoMember>()) {
                    return false;
                }
                int keyIndex = findMemberIndexInKeys(flatMapLambdaArg, item.Cast<TCoMember>());
                if (keyIndex >= 0) {
                    usedKeyIndexes.push_back(keyIndex);
                } else {
                    return false;
                }
            }
            if (usedKeyIndexes.empty()) {
                return false;
            }
            ::Sort(usedKeyIndexes);
            for (size_t i = 0; i < usedKeyIndexes.size(); ++i) {
                if (usedKeyIndexes[i] != (int) i) {
                    return false;
                }
            }
        } else {
            return false;
        }

        return CanRewriteSqlInToEquiJoin(sqlIn.Lookup().Ref().GetTypeAnn(), sqlIn.Collection().Ref().GetTypeAnn());
    };

    const bool prefixOnly = true;
    if (auto ret = TryConvertSqlInPredicatesToJoins(flatMap, shouldConvertSqlInToJoin, ctx, prefixOnly)) {
        return TExprBase(ret);
    }

    return node;
}

} // namespace NKikimr::NKqp::NOpt 
