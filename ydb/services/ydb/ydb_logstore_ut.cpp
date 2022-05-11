#include "ydb_common_ut.h"

#include <ydb/public/sdk/cpp/client/ydb_result/result.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>
#include <ydb/public/lib/experimental/ydb_logstore.h>

#include <ydb/library/yql/public/issue/yql_issue.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>

using namespace NYdb;

namespace {

TVector<NYdb::TColumn> TestSchemaColumns() {
    return {
        NYdb::TColumn("timestamp",      NYdb::NLogStore::MakeColumnType(EPrimitiveType::Timestamp)),
        NYdb::TColumn("resource_type",  NYdb::NLogStore::MakeColumnType(EPrimitiveType::Utf8)),
        NYdb::TColumn("resource_id",    NYdb::NLogStore::MakeColumnType(EPrimitiveType::Utf8)),
        NYdb::TColumn("uid",            NYdb::NLogStore::MakeColumnType(EPrimitiveType::Utf8)),
        NYdb::TColumn("level",          NYdb::NLogStore::MakeColumnType(EPrimitiveType::Int32)),
        NYdb::TColumn("message",        NYdb::NLogStore::MakeColumnType(EPrimitiveType::Utf8)),
        NYdb::TColumn("json_payload",   NYdb::NLogStore::MakeColumnType(EPrimitiveType::JsonDocument)),
        NYdb::TColumn("request_id",     NYdb::NLogStore::MakeColumnType(EPrimitiveType::Utf8)),
        NYdb::TColumn("ingested_at",    NYdb::NLogStore::MakeColumnType(EPrimitiveType::Timestamp)),
        NYdb::TColumn("saved_at",       NYdb::NLogStore::MakeColumnType(EPrimitiveType::Timestamp)),
    };
}

TVector<TString> TestSchemaKey() {
    return {"timestamp", "resource_type", "resource_id", "uid"};
}

THashMap<TString, NYdb::NLogStore::TTierConfig> TestTierConfigs() {
    using NYdb::NLogStore::TTierConfig;
    using NYdb::NLogStore::TCompression;
    using NYdb::NLogStore::EColumnCompression;

    THashMap<TString, TTierConfig> out;
    out.emplace("default", TTierConfig{ TCompression{ EColumnCompression::LZ4, {}} });
    out.emplace("tier_zstd1", TTierConfig{ TCompression{ EColumnCompression::ZSTD, 1} });
    out.emplace("tier_zstd5", TTierConfig{ TCompression{ EColumnCompression::ZSTD, 5} });
    return out;
}

}

Y_UNIT_TEST_SUITE(YdbLogStore) {

    void EnableDebugLogs(TKikimrWithGrpcAndRootSchema& server) {
        server.Server_->GetRuntime()->SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_DEBUG);
        server.Server_->GetRuntime()->SetLogPriority(NKikimrServices::TX_COLUMNSHARD, NActors::NLog::PRI_DEBUG);
        server.Server_->GetRuntime()->SetLogPriority(NKikimrServices::MSGBUS_REQUEST, NActors::NLog::PRI_DEBUG);
    }

    NYdb::TDriver ConnectToServer(TKikimrWithGrpcAndRootSchema& server, const TString& token = {}) {
        ui16 grpc = server.GetPort();
        TString location = TStringBuilder() << "localhost:" << grpc;
        auto connection = NYdb::TDriver(TDriverConfig().SetEndpoint(location).SetDatabase("/Root").SetAuthToken(token));
        return connection;
    }

    Y_UNIT_TEST(LogStore) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableOlapSchemaOperations(true);
        TKikimrWithGrpcAndRootSchema server(appConfig);
        EnableDebugLogs(server);

        auto connection = ConnectToServer(server);
        NYdb::NLogStore::TLogStoreClient logStoreClient(connection);

        {
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), TestSchemaKey());
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DescribeLogStore("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            UNIT_ASSERT_VALUES_EQUAL(descr.GetColumnShardCount(), 4);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetSchemaPresets().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetSchemaPresets().count("default"), 1);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetOwner(), "root@builtin");

            const auto& schema = descr.GetSchemaPresets().begin()->second;
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns().size(), 10);
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[0].ToString(), "{ name: \"timestamp\", type: Timestamp? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[1].ToString(), "{ name: \"resource_type\", type: Utf8? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[4].ToString(), "{ name: \"level\", type: Int32? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetPrimaryKeyColumns(),
                TVector<TString>({"timestamp", "resource_type", "resource_id", "uid"}));
            UNIT_ASSERT_EQUAL(schema.GetDefaultCompression().Codec, NYdb::NLogStore::EColumnCompression::LZ4);
            UNIT_ASSERT(!schema.GetDefaultCompression().Level);
        }

        {
            auto res = logStoreClient.DropLogStore("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(LogStoreTiers) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableOlapSchemaOperations(true);
        TKikimrWithGrpcAndRootSchema server(appConfig);
        EnableDebugLogs(server);

        auto connection = ConnectToServer(server);
        NYdb::NLogStore::TLogStoreClient logStoreClient(connection);

        {
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), TestSchemaKey(),
                NYdb::NLogStore::TCompression{NYdb::NLogStore::EColumnCompression::ZSTD, 1});
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets, TestTierConfigs());
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DescribeLogStore("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            UNIT_ASSERT_VALUES_EQUAL(descr.GetColumnShardCount(), 4);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetSchemaPresets().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetSchemaPresets().count("default"), 1);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetOwner(), "root@builtin");

            const auto& schema = descr.GetSchemaPresets().begin()->second;
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns().size(), 10);
            UNIT_ASSERT_VALUES_EQUAL(schema.GetPrimaryKeyColumns().size(), 4);
            UNIT_ASSERT_EQUAL(schema.GetDefaultCompression().Codec, NYdb::NLogStore::EColumnCompression::ZSTD);
            UNIT_ASSERT_VALUES_EQUAL(schema.GetDefaultCompression().Level, 1);

            const auto& tiers = descr.GetTierConfigs();
            auto expectedTiers = TestTierConfigs();
            UNIT_ASSERT_VALUES_EQUAL(tiers.size(), expectedTiers.size());

            for (auto& [name, cfg] : expectedTiers) {
                UNIT_ASSERT_VALUES_EQUAL(tiers.count(name), 1);
                UNIT_ASSERT_EQUAL(tiers.find(name)->second.Compression.Codec, cfg.Compression.Codec);
                UNIT_ASSERT_VALUES_EQUAL(tiers.find(name)->second.Compression.Level, cfg.Compression.Level);
            }
        }

        {
            auto res = logStoreClient.DropLogStore("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(LogStoreNegative) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableOlapSchemaOperations(true);
        TKikimrWithGrpcAndRootSchema server(appConfig);
        EnableDebugLogs(server);

        auto connection = ConnectToServer(server);
        NYdb::NLogStore::TLogStoreClient logStoreClient(connection);

        { // wrong schema: no columns
            NYdb::NLogStore::TSchema logSchema({}, TestSchemaKey());
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        { // wrong schema: no PK
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), {});
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        { // wrong schema: wrong PK
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), {"timestamp", "unknown_column"});
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        { // wrong schema: not supported PK
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), {"resource_type", "resource_id"});
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        { // no "default" preset
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), TestSchemaKey());
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["some"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        { // Compression::None is not tested yet - disabled
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), TestSchemaKey(),
                NYdb::NLogStore::TCompression{NYdb::NLogStore::EColumnCompression::None, {}});
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(Dirs) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableOlapSchemaOperations(true);
        TKikimrWithGrpcAndRootSchema server(appConfig);
        EnableDebugLogs(server);

        auto connection = ConnectToServer(server);
        NYdb::NLogStore::TLogStoreClient logStoreClient(connection);

        {
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), TestSchemaKey());
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/home/folder/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DescribeLogStore("/Root/home/folder/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        NYdb::NScheme::TSchemeClient schemeClient(connection);

        // MkDir inside LogStore
        {
            auto res = schemeClient.MakeDirectory("/Root/home/folder/LogStore/Dir1").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        // Re-create the same dir
        {
            auto res = schemeClient.MakeDirectory("/Root/home/folder/LogStore/Dir1").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        // MkDir for existing LogStore path
        {
            auto res = schemeClient.MakeDirectory("/Root/home/folder/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        // Two levels of non-existing dirs
        {
            auto res = schemeClient.MakeDirectory("/Root/home/folder/LogStore/Dir2/Dir3").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        // Log table with intermediate dirs
        {
            NYdb::NLogStore::TLogTableDescription tableDescr("default", {"timestamp", "uid"}, 4);
            auto res = logStoreClient.CreateLogTable("/Root/home/folder/LogStore/Dir1/Dir2/log1", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(LogTable) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableOlapSchemaOperations(true);
        TKikimrWithGrpcAndRootSchema server(appConfig);
        EnableDebugLogs(server);

        auto connection = ConnectToServer(server);
        NYdb::NLogStore::TLogStoreClient logStoreClient(connection);

        NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), TestSchemaKey());

        {
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            NYdb::NLogStore::TLogTableDescription tableDescr("default", {"timestamp", "uid"}, 4);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log1", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log1").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            UNIT_ASSERT_VALUES_EQUAL(descr.GetColumnShardCount(), 4);
            const auto& schema = descr.GetSchema();
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns().size(), 10);
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[0].ToString(), "{ name: \"timestamp\", type: Timestamp? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[1].ToString(), "{ name: \"resource_type\", type: Utf8? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[4].ToString(), "{ name: \"level\", type: Int32? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetPrimaryKeyColumns(),
                TVector<TString>({"timestamp", "resource_type", "resource_id", "uid"}));
            UNIT_ASSERT_VALUES_EQUAL(descr.GetOwner(), "root@builtin");
        }

        {
            NYdb::NLogStore::TLogTableDescription tableDescr(logSchema, {"timestamp", "uid"}, 4);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log2", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log2").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            UNIT_ASSERT_VALUES_EQUAL(descr.GetColumnShardCount(), 4);
            const auto& schema = descr.GetSchema();
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns().size(), 10);
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[0].ToString(), "{ name: \"timestamp\", type: Timestamp? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[1].ToString(), "{ name: \"resource_type\", type: Utf8? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetColumns()[4].ToString(), "{ name: \"level\", type: Int32? }");
            UNIT_ASSERT_VALUES_EQUAL(schema.GetPrimaryKeyColumns(),
                TVector<TString>({"timestamp", "resource_type", "resource_id", "uid"}));
            UNIT_ASSERT_VALUES_EQUAL(descr.GetOwner(), "root@builtin");
        }

        {
            NYdb::NLogStore::TLogTableDescription tableDescr(logSchema, {"timestamp", "uid"}, 4);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log2", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            NYdb::NScheme::TSchemeClient schemaClient(connection);
            auto res = schemaClient.ListDirectory("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto children = res.GetChildren();
            UNIT_ASSERT_VALUES_EQUAL(children.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(children[0].Name, "log1");
            UNIT_ASSERT_VALUES_EQUAL(children[1].Name, "log2");
            UNIT_ASSERT_VALUES_EQUAL(children[2].Name, ".sys");
        }

        {
            NYdb::NScheme::TSchemeClient schemaClient(connection);
            auto res = schemaClient.ListDirectory("/Root/LogStore/.sys").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto children = res.GetChildren();
            UNIT_ASSERT_VALUES_EQUAL(children.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(children[0].Name, "store_primary_index_stats");
        }

        {
            NYdb::NScheme::TSchemeClient schemaClient(connection);
            auto res = schemaClient.ListDirectory("/Root/LogStore/log1").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto children = res.GetChildren();
            UNIT_ASSERT_VALUES_EQUAL(children.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(children[0].Name, ".sys");
        }

        {
            NYdb::NScheme::TSchemeClient schemaClient(connection);
            auto res = schemaClient.ListDirectory("/Root/LogStore/log1/.sys").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto children = res.GetChildren();
            UNIT_ASSERT_VALUES_EQUAL(children.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(children[0].Name, "primary_index_stats");
        }

        {
            // Try to drop non-empty LogStore
            auto res = logStoreClient.DropLogStore("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DropLogTable("/Root/LogStore/log1").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            // Try to drop LogTable as LogStore
            auto res = logStoreClient.DropLogStore("/Root/LogStore/log2").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DropLogTable("/Root/LogStore/log2").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        {
            // Try to drop LogStore as LogTable
            auto res = logStoreClient.DropLogTable("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SCHEME_ERROR, res.GetIssues().ToString());
        }

        {
            auto res = logStoreClient.DropLogStore("/Root/LogStore").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(AlterLogStore) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableOlapSchemaOperations(true);
        TKikimrWithGrpcAndRootSchema server(appConfig);
        EnableDebugLogs(server);

        auto connection = ConnectToServer(server);
        NYdb::NLogStore::TLogStoreClient logStoreClient(connection);

        // Add LogStore (currently not supported)
        {
            NYdb::NLogStore::TAlterLogStoreSettings alterLogStoreSettings;
            auto res = logStoreClient.AlterLogStore("/Root/LogStore", std::move(alterLogStoreSettings)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::UNSUPPORTED, res.GetIssues().ToString());
        }
    }

    Y_UNIT_TEST(AlterLogTable) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableOlapSchemaOperations(true);
        TKikimrWithGrpcAndRootSchema server(appConfig);
        EnableDebugLogs(server);

        auto connection = ConnectToServer(server);
        NYdb::NLogStore::TLogStoreClient logStoreClient(connection);

        {
            NYdb::NLogStore::TSchema logSchema(TestSchemaColumns(), TestSchemaKey());
            THashMap<TString, NYdb::NLogStore::TSchema> schemaPresets;
            schemaPresets["default"] = logSchema;
            NYdb::NLogStore::TLogStoreDescription storeDescr(4, schemaPresets);
            auto res = logStoreClient.CreateLogStore("/Root/LogStore", std::move(storeDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }

        // Create table without TTL settings
        {
            NYdb::NLogStore::TLogTableDescription tableDescr("default", {"timestamp", "uid"}, 4);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log1", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }
        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log1").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            UNIT_ASSERT_C(!descr.GetTtlSettings(), "The table was created without TTL settings");
        }

        // Create table with TTL settings
        {
            NYdb::NLogStore::TTtlSettings ttlSettings("saved_at", TDuration::Seconds(2000));
            NYdb::NLogStore::TLogTableDescription tableDescr("default", {"timestamp", "uid"}, 4, ttlSettings);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log2", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }
        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log2").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            auto ttlSettings = descr.GetTtlSettings();
            UNIT_ASSERT_C(!ttlSettings.Empty(), "The table was created with TTL settings");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetColumnName(), "saved_at");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetExpireAfter(), TDuration::Seconds(2000));
        }

        // Add TTL to a table (currently not supported)
        {
            NYdb::NLogStore::TAlterLogTableSettings alterLogTableSettings;
            alterLogTableSettings.AlterTtlSettings(NYdb::NTable::TAlterTtlSettings::Set("uint_timestamp", NYdb::NTable::TTtlSettings::EUnit::MilliSeconds, TDuration::Seconds(3600)));
            auto res = logStoreClient.AlterLogTable("/Root/LogStore/log1", std::move(alterLogTableSettings)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::GENERIC_ERROR, res.GetIssues().ToString());
        }
        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log1").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            auto ttlSettings = descr.GetTtlSettings();
            UNIT_ASSERT_C(ttlSettings.Empty(), "Table must not have TTL settings");
        }

        // Change TTL column (currently not supported)
        {
            NYdb::NLogStore::TAlterLogTableSettings alterLogTableSettings;
            alterLogTableSettings.AlterTtlSettings(NYdb::NTable::TAlterTtlSettings::Set("ingested_at", TDuration::Seconds(86400)));
            auto res = logStoreClient.AlterLogTable("/Root/LogStore/log2", std::move(alterLogTableSettings)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::GENERIC_ERROR, res.GetIssues().ToString());
        }
        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log2").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            auto ttlSettings = descr.GetTtlSettings();
            UNIT_ASSERT_C(!ttlSettings.Empty(), "Table must have TTL settings");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetColumnName(), "saved_at");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetExpireAfter(), TDuration::Seconds(2000));
        }

        // Change TTL expiration time
        {
            NYdb::NLogStore::TAlterLogTableSettings alterLogTableSettings;
            alterLogTableSettings.AlterTtlSettings(NYdb::NTable::TAlterTtlSettings::Set("saved_at", TDuration::Seconds(86400)));
            auto res = logStoreClient.AlterLogTable("/Root/LogStore/log2", std::move(alterLogTableSettings)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
        }
        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log2").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            auto ttlSettings = descr.GetTtlSettings();
            UNIT_ASSERT_C(!ttlSettings.Empty(), "Table must have TTL settings");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetColumnName(), "saved_at");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetExpireAfter(), TDuration::Seconds(86400));
        }

        // Remove TTL (currently not supported)
        {
            NYdb::NLogStore::TAlterLogTableSettings alterLogTableSettings;
            alterLogTableSettings.AlterTtlSettings(NYdb::NTable::TAlterTtlSettings::Drop());
            auto res = logStoreClient.AlterLogTable("/Root/LogStore/log2", std::move(alterLogTableSettings)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::GENERIC_ERROR, res.GetIssues().ToString());
        }
        {
            auto res = logStoreClient.DescribeLogTable("/Root/LogStore/log2").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::SUCCESS, res.GetIssues().ToString());
            auto descr = res.GetDescription();
            auto ttlSettings = descr.GetTtlSettings();
            UNIT_ASSERT_C(!ttlSettings.Empty(), "Table must have TTL settings");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetColumnName(), "saved_at");
            UNIT_ASSERT_VALUES_EQUAL(ttlSettings->GetDateTypeColumn().GetExpireAfter(), TDuration::Seconds(86400));
        }

        // Use invalid column for TTL
        {
            NYdb::NLogStore::TTtlSettings ttlSettings("nonexisting_column", TDuration::Seconds(2000));
            NYdb::NLogStore::TLogTableDescription tableDescr("default", {"timestamp", "uid"}, 4, ttlSettings);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log3", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::GENERIC_ERROR, res.GetIssues().ToString());
        }

        // Use column of invalid type for TTL
        {
            NYdb::NLogStore::TTtlSettings ttlSettings("message", NYdb::NTable::TTtlSettings::EUnit::MilliSeconds, TDuration::Seconds(3600));
            NYdb::NLogStore::TLogTableDescription tableDescr("default", {"timestamp", "uid"}, 4, ttlSettings);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log4", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::GENERIC_ERROR, res.GetIssues().ToString());
        }

        // Use non-Timestamp column for TTL
        {
            NYdb::NLogStore::TTtlSettings ttlSettings("uint_timestamp", NYdb::NTable::TTtlSettings::EUnit::MilliSeconds, TDuration::Seconds(3600));
            NYdb::NLogStore::TLogTableDescription tableDescr("default", {"timestamp", "uid"}, 4, ttlSettings);
            auto res = logStoreClient.CreateLogTable("/Root/LogStore/log5", std::move(tableDescr)).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(res.GetStatus(), EStatus::GENERIC_ERROR, res.GetIssues().ToString());
        }
    }
}
