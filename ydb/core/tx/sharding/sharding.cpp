#include "sharding.h"
#include "xx_hash.h"
#include <ydb/library/yql/utils/yql_panic.h>
#include <ydb/library/yql/minikql/mkql_node.h>
#include <util/string/join.h>

namespace NKikimr::NSharding {

void TShardingBase::AppendField(const std::shared_ptr<arrow::Array>& array, int row, IHashCalcer& hashCalcer) {
    NArrow::SwitchType(array->type_id(), [&](const auto& type) {
        using TWrap = std::decay_t<decltype(type)>;
        using T = typename TWrap::T;
        using TArray = typename arrow::TypeTraits<T>::ArrayType;

        if (!array->IsNull(row)) {
            auto& typedArray = static_cast<const TArray&>(*array);
            auto value = typedArray.GetView(row);
            if constexpr (arrow::has_string_view<T>()) {
                hashCalcer.Update((const ui8*)value.data(), value.size());
            } else if constexpr (arrow::has_c_type<T>()) {
                if constexpr (arrow::is_physical_integer_type<T>()) {
                    hashCalcer.Update(reinterpret_cast<const ui8*>(&value), sizeof(value));
                } else {
                    // Do not use bool or floats for sharding
                    static_assert(arrow::is_boolean_type<T>() || arrow::is_floating_type<T>());
                }
            } else {
                static_assert(arrow::is_decimal_type<T>());
            }
        }
        return true;
        });
}

void TUnboxedValueReader::BuildStringForHash(const NKikimr::NUdf::TUnboxedValue& value, IHashCalcer& hashCalcer) const {
    for (auto&& i : ColumnsInfo) {
        auto columnValue = value.GetElement(i.Idx);
        if (columnValue.IsString()) {
            hashCalcer.Update((const ui8*)columnValue.AsStringRef().Data(), columnValue.AsStringRef().Size());
        } else if (columnValue.IsEmbedded()) {
            switch (i.Type.GetTypeId()) {
                case NScheme::NTypeIds::Uint16:
                    FieldToHashString<ui16>(columnValue, hashCalcer);
                    continue;
                case NScheme::NTypeIds::Uint32:
                    FieldToHashString<ui32>(columnValue, hashCalcer);
                    continue;
                case NScheme::NTypeIds::Uint64:
                    FieldToHashString<ui64>(columnValue, hashCalcer);
                    continue;
                case NScheme::NTypeIds::Int16:
                    FieldToHashString<i16>(columnValue, hashCalcer);
                    continue;
                case NScheme::NTypeIds::Int32:
                    FieldToHashString<i32>(columnValue, hashCalcer);
                    continue;
                case NScheme::NTypeIds::Int64:
                    FieldToHashString<i64>(columnValue, hashCalcer);
                    continue;
            }
            YQL_ENSURE(false, "incorrect column type for shard calculation");
        } else {
            YQL_ENSURE(false, "incorrect column type for shard calculation");
        }
    }
}

TUnboxedValueReader::TUnboxedValueReader(const NMiniKQL::TStructType* structInfo,
    const TMap<TString, TExternalTableColumn>& columnsRemap, const std::vector<TString>& shardingColumns) {
    YQL_ENSURE(shardingColumns.size());
    for (auto&& i : shardingColumns) {
        auto it = columnsRemap.find(i);
        YQL_ENSURE(it != columnsRemap.end());
        ColumnsInfo.emplace_back(TColumnUnboxedPlaceInfo(it->second, structInfo->GetMemberIndex(i), i));
    }
}

std::unique_ptr<TShardingBase> TShardingBase::BuildShardingOperator(const NKikimrSchemeOp::TColumnTableSharding& sharding) {
    if (sharding.HasHashSharding()) {
        auto& hashSharding = sharding.GetHashSharding();
        std::vector<TString> columnNames(hashSharding.GetColumns().begin(), hashSharding.GetColumns().end());
        if (hashSharding.GetFunction() == NKikimrSchemeOp::TColumnTableSharding::THashSharding::HASH_FUNCTION_MODULO_N) {
            return std::make_unique<THashSharding>(sharding.GetColumnShards().size(), columnNames, 0);
        } else if (hashSharding.GetFunction() == NKikimrSchemeOp::TColumnTableSharding::THashSharding::HASH_FUNCTION_CLOUD_LOGS) {
            ui32 activeShards = TLogsSharding::DEFAULT_ACITVE_SHARDS;
            if (hashSharding.HasActiveShardsCount()) {
                activeShards = hashSharding.GetActiveShardsCount();
            }
            return std::make_unique<TLogsSharding>(sharding.GetColumnShards().size(), columnNames, activeShards);
        }
    }
    return nullptr;
}

TString TShardingBase::DebugString() const {
    return "Columns: " + JoinSeq(", ", GetShardingColumns());
}


std::vector<ui32> THashSharding::MakeSharding(const std::shared_ptr<arrow::RecordBatch>& batch) const {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(ShardingColumns.size());

    for (auto& colName : ShardingColumns) {
        auto array = batch->GetColumnByName(colName);
        if (!array) {
            return {};
        }
        columns.emplace_back(array);
    }

    std::vector<ui32> out(batch->num_rows());

    TStreamStringHashCalcer hashCalcer(Seed);

    for (int row = 0; row < batch->num_rows(); ++row) {
        hashCalcer.Start();
        for (auto& column : columns) {
            AppendField(column, row, hashCalcer);
        }
        out[row] = hashCalcer.Finish() % ShardsCount;
    }

    return out;
}

ui32 THashSharding::CalcShardId(const NKikimr::NUdf::TUnboxedValue& value, const TUnboxedValueReader& readerInfo) const {
    TStreamStringHashCalcer hashCalcer(Seed);
    hashCalcer.Start();
    readerInfo.BuildStringForHash(value, hashCalcer);
    return hashCalcer.Finish() % ShardsCount;
}

std::vector<ui32> TLogsSharding::MakeSharding(const std::shared_ptr<arrow::RecordBatch>& batch) const {
    if (ShardingColumns.size() < 2) {
        return {};
    }

    auto tsArray = batch->GetColumnByName(ShardingColumns[0]);
    if (!tsArray || tsArray->type_id() != arrow::Type::TIMESTAMP) {
        return {};
    }

    std::vector<std::shared_ptr<arrow::Array>> extraColumns;
    extraColumns.reserve(ShardingColumns.size() - 1);

    for (size_t i = 1; i < ShardingColumns.size(); ++i) {
        auto array = batch->GetColumnByName(ShardingColumns[i]);
        if (!array) {
            return {};
        }
        extraColumns.emplace_back(array);
    }

    auto tsColumn = std::static_pointer_cast<arrow::TimestampArray>(tsArray);
    std::vector<ui32> out;
    out.reserve(batch->num_rows());

    TStreamStringHashCalcer hashCalcer(0);
    for (int row = 0; row < batch->num_rows(); ++row) {
        hashCalcer.Start();
        for (auto& column : extraColumns) {
            AppendField(column, row, hashCalcer);
        }

        const ui32 shardNo = ShardNo(tsColumn->Value(row), hashCalcer.Finish());
        out.emplace_back(shardNo);
    }

    return out;
}

ui32 TLogsSharding::CalcShardId(const NKikimr::NUdf::TUnboxedValue& /*value*/, const TUnboxedValueReader& /*readerInfo*/) const {
    YQL_ENSURE(false);
    return 0;
}

}
