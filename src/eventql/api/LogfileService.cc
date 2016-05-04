/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include <unistd.h>
#include "eventql/api/LogfileService.h"
#include "eventql/util/RegExp.h"
#include "eventql/util/human.h"
#include "eventql/util/protobuf/msg.h"
#include "eventql/util/protobuf/MessageSchema.h"
#include "eventql/util/protobuf/MessagePrinter.h"
#include "eventql/util/protobuf/MessageEncoder.h"
#include "eventql/util/protobuf/DynamicMessage.h"
#include "eventql/sql/qtree/SelectListNode.h"
#include "eventql/sql/qtree/ColumnReferenceNode.h"
#include "eventql/sql/CSTableScan.h"
#include "eventql/core/TimeWindowPartitioner.h"
#include "eventql/core/SQLEngine.h"

using namespace stx;

namespace zbase {

LogfileService::LogfileService(
    ConfigDirectory* cdir,
    AnalyticsAuth* auth,
    zbase::TSDBService* tsdb,
    zbase::PartitionMap* pmap,
    zbase::ReplicationScheme* repl,
    csql::Runtime* sql) :
    cdir_(cdir),
    auth_(auth),
    tsdb_(tsdb),
    pmap_(pmap),
    repl_(repl),
    sql_(sql) {}

void LogfileService::scanLogfile(
    const AnalyticsSession& session,
    const String& logfile_name,
    const LogfileScanParams& params,
    LogfileScanResult* result,
    Function<void (bool done)> on_progress) {
  auto logfile_definition = findLogfileDefinition(
      session.customer(), logfile_name);
  if (logfile_definition.isEmpty()) {
    RAISEF(kNotFoundError, "logfile not found: $0", logfile_name);
  }

  auto table_name = "logs." + logfile_name;
  auto lookback_limit = params.end_time() - 90 * kMicrosPerDay;
  auto partition_size = 10 * kMicrosPerMinute;

  result->setColumns(
      Vector<String>(params.columns().begin(), params.columns().end()));

  for (auto time = params.end_time();
      time > lookback_limit;
      time -= partition_size) {

    auto partition = zbase::TimeWindowPartitioner::partitionKeyFor(
        table_name,
        time,
        partition_size);

    if (repl_->hasLocalReplica(partition)) {
      scanLocalLogfilePartition(
          session,
          table_name,
          partition,
          params,
          result);
    } else {
      scanRemoteLogfilePartition(
          session,
          table_name,
          partition,
          params,
          repl_->replicaAddrsFor(partition),
          result);
    }

    result->setScannedUntil(time);

    bool done = result->isFull();
    on_progress(done);

    if (done) {
      break;
    }
  }
}

void LogfileService::scanLocalLogfilePartition(
    const AnalyticsSession& session,
    const String& table_name,
    const SHA1Hash& partition_key,
    const LogfileScanParams& params,
    LogfileScanResult* result) {
  auto partition = pmap_->findPartition(
      session.customer(),
      table_name,
      partition_key);

  if (partition.isEmpty()) {
    return;
  }

  logDebug(
      "zbase",
      "Scanning local logfile partition $0/$1/$2",
      session.customer(),
      table_name,
      partition_key.toString());

  auto txn = sql_->newTransaction();
  auto tables = zbase::SQLEngine::tableProviderForNamespace(
        pmap_,
        repl_,
        nullptr,
        auth_,
        session.customer());

  auto table_info = tables->describe(table_name);
  if (table_info.isEmpty()) {
    RAISEF(kNotFoundError, "table not found: '$0'", table_name);
  }

  Vector<RefPtr<csql::SelectListNode>> select_list;
  select_list.emplace_back(
      new csql::SelectListNode(
          new csql::ColumnReferenceNode("time")));

  if (params.return_raw()) {
    select_list.emplace_back(
        new csql::SelectListNode(
            new csql::ColumnReferenceNode("raw")));
  }

  for (const auto& c : params.columns()) {
    select_list.emplace_back(
        new csql::SelectListNode(
            new csql::ColumnReferenceNode(c)));
  }

  Option<RefPtr<csql::ValueExpressionNode>> where_cond;
  switch (params.scan_type()) {

    case LOGSCAN_SQL: {
      const auto& sql_str = params.condition();

      csql::Parser parser;
      parser.parseValueExpression(sql_str.data(), sql_str.size());

      auto stmts = parser.getStatements();
      if (stmts.size() != 1) {
        RAISE(
            kParseError,
            "SQL filter expression must consist of exactly one statement");
      }

      where_cond = Some(
          sql_->queryPlanBuilder()->buildValueExpression(txn.get(), stmts[0]));

      break;
    }

  }

  auto seqscan = mkRef(
      new csql::SequentialScanNode(
          table_info.get(),
          select_list,
          where_cond));

  auto reader = partition.get()->getReader();
  auto cstable_filename = reader->cstableFilename();
  if (cstable_filename.isEmpty()) {
    return;
  }

  csql::CSTableScan cstable_scan(
      txn.get(),
      seqscan,
      cstable_filename.get(),
      sql_->queryBuilder().get());

  csql::ExecutionContext context(sql_->scheduler());

  cstable_scan.execute(
      &context,
      [result, &params] (int argc, const csql::SValue* argv) -> bool {
    int colidx = 0;

    auto time = UnixTime(argv[colidx++].getInteger());
    if (time >= params.end_time()) {
      return true;
    }

    auto line = result->addLine(time);
    if (!line) {
      return true;
    }

    if (params.return_raw()) {
      line->raw = argv[colidx++].getString();
    }

    for (; colidx < argc; ++colidx) {
      line->columns.emplace_back(argv[colidx].getString());
    }

    return true;
  });

  result->incrRowsScanned(cstable_scan.rowsScanned());
}

void LogfileService::scanRemoteLogfilePartition(
    const AnalyticsSession& session,
    const String& table_name,
    const SHA1Hash& partition_key,
    const LogfileScanParams& params,
    const Vector<InetAddr>& hosts,
    LogfileScanResult* result) {
  Vector<String> errors;

  for (const auto& host : hosts) {
    try {
      if(scanRemoteLogfilePartition(
          session,
          table_name,
          partition_key,
          params,
          host,
          result)) {
        return;
      }
    } catch (const StandardException& e) {
      logError(
          "zbase",
          e,
          "LogfileService::scanRemoteLogfilePartition failed");

      errors.emplace_back(e.what());
    }
  }

  if (!errors.empty()) {
    RAISEF(
        kRuntimeError,
        "LogfileService::scanRemoteLogfilePartition failed: $0",
        StringUtil::join(errors, ", "));
  }
}

bool LogfileService::scanRemoteLogfilePartition(
    const AnalyticsSession& session,
    const String& table_name,
    const SHA1Hash& partition_key,
    const LogfileScanParams& params,
    const InetAddr& host,
    LogfileScanResult* result) {
  logDebug(
      "zbase",
      "Scanning remote logfile partition $0/$1/$2 on $3",
      session.customer(),
      table_name,
      partition_key.toString(),
      host.hostAndPort());

  auto url = StringUtil::format(
      "http://$0/api/v1/logfiles/scan_partition?table=$1&partition=$2&limit=$3",
      host.hostAndPort(),
      URI::urlEncode(table_name),
      partition_key.toString(),
      result->capacity());

  auto api_token = auth_->encodeAuthToken(session);

  http::HTTPMessage::HeaderList auth_headers;
  auth_headers.emplace_back(
      "Authorization",
      StringUtil::format("Token $0", api_token));

  http::HTTPClient http_client(&z1stats()->http_client_stats);
  auto req_body = msg::encode(params);
  auto req = http::HTTPRequest::mkPost(url, *req_body, auth_headers);
  auto res = http_client.executeRequest(req);

  if (res.statusCode() == 404) {
    return false;
  }

  if (res.statusCode() != 200) {
    RAISEF(
        kRuntimeError,
        "received non-200 response: $0", res.body().toString());
  }

  auto body_is = BufferInputStream::fromBuffer(&res.body());
  result->decode(body_is.get());

  return true;
}

void LogfileService::insertLoglines(
    const String& customer,
    const String& logfile_name,
    const Vector<Pair<String, String>>& source_fields,
    InputStream* is) {
  auto logfile_definition = findLogfileDefinition(customer, logfile_name);
  if (logfile_definition.isEmpty()) {
    RAISEF(kNotFoundError, "logfile not found: $0", logfile_name);
  }

  insertLoglines(
      customer,
      logfile_definition.get(),
      source_fields,
      is);
}

void LogfileService::insertLoglines(
    const String& customer,
    const LogfileDefinition& logfile,
    const Vector<Pair<String, String>>& source_fields,
    InputStream* is) {
  String line;

  auto table_name = "logs." + logfile.name();
  auto schema = tsdb_->tableSchema(customer, table_name);
  auto partitioner = tsdb_->tablePartitioner(customer, table_name);
  if (schema.isEmpty() || partitioner.isEmpty()) {
    RAISEF(kNotFoundError, "table not found: $0", table_name);
  }

  msg::DynamicMessage row_base(schema.get());
  for (const auto& f : source_fields) {
    row_base.addField(f.first, f.second);
  }

  RegExp regex(logfile.regex());

  HashMap<size_t, LogfileField> match_fields;
  size_t time_idx = -1;
  String time_format;
  for (const auto& f : logfile.row_fields()) {
    auto match_idx = regex.getNamedCaptureIndex(f.name());
    if (match_idx != size_t(-1)) {
      match_fields.emplace(match_idx, f);
      if (f.name() == "time") {
        time_idx = match_idx;
        time_format = f.format();
      }
    }
  }

  if (time_idx == size_t(-1)) {
    RAISE(kIllegalStateError, "can't import logfile row without time column");
  }

  Vector<RecordEnvelope> records;
  static const size_t kInsertBatchSize = 1024;

  for (; is->readLine(&line); line.clear()) {
    Vector<Pair<const char*, size_t>> match;
    if (!regex.match(line, &match)) {
      logTrace(
          "z1.logs",
          "Dropping logline for logfile '$0/$1' because it does not match " \
          "the parsing regex: $2",
          customer,
          logfile.name(),
          line);

      continue;
    }

    Option<UnixTime> time;
    if (time_format.empty()) {
      time = Human::parseTime(
          String(match[time_idx].first, match[time_idx].second));
    } else {
      time = UnixTime::parseString(
          match[time_idx].first,
          match[time_idx].second,
          time_format.c_str());
    }

    if (time.isEmpty()) {
      logTrace(
          "z1.logs",
          "Dropping logline for logfile '$0/$1' because it does not have a " \
          "'time' column: $2",
          customer,
          logfile.name(),
          line);

      continue;
    }

    auto row = row_base;
    row.addField("raw", line);

    for (size_t i = 0; i < match.size(); ++i) {
      auto mfield = match_fields.find(i);
      if (mfield == match_fields.end()) {
        continue;
      }

      if (mfield->second.has_format() &&
          mfield->second.type() == "DATETIME") {
        auto t = UnixTime::parseString(
            match[i].first,
            match[i].second,
            mfield->second.format().c_str());

        if (!t.isEmpty()) {
          row.addDateTimeField(mfield->second.id(), t.get());
        }
        continue;
      }

      row.addField(
          mfield->second.id(),
          String(match[i].first, match[i].second));
    }

    Buffer row_buf;
    msg::MessageEncoder::encode(row.data(), *row.schema(), &row_buf);

    auto record_id = Random::singleton()->sha1();
    auto partition_key = partitioner.get()->partitionKeyFor(
        StringUtil::toString(time.get().unixMicros()));

    RecordEnvelope envelope;
    envelope.set_tsdb_namespace(customer);
    envelope.set_table_name(table_name);
    envelope.set_partition_sha1(partition_key.toString());
    envelope.set_record_id(record_id.toString());
    envelope.set_record_data(row_buf.toString());
    records.emplace_back(envelope);

    if (records.size() > kInsertBatchSize) {
      tsdb_->insertRecords(records);
      records.clear();
    }
  }

  if (records.size() > 0) {
    tsdb_->insertRecords(records);
  }
}

Option<LogfileDefinition> LogfileService::findLogfileDefinition(
    const String& customer,
    const String& logfile_name) {
  auto cconf = cdir_->configFor(customer);

  for (const auto& logfile : cconf->config.logfile_import_config().logfiles()) {
    if (logfile.name() == logfile_name) {
      return Some(logfile);
    }
  }

  return None<LogfileDefinition>();
}

void LogfileService::setLogfileRegex(
    const String& customer,
    const String& logfile_name,
    const String& regex) {
  auto cconf = cdir_->configFor(customer)->config;
  auto logfile_conf = cconf.mutable_logfile_import_config();

  for (auto& logfile : *logfile_conf->mutable_logfiles()) {
    if (logfile.name() == logfile_name) {
      logfile.set_regex(regex);
      cdir_->updateCustomerConfig(cconf);
      return;
    }
  }

  RAISEF(kNotFoundError, "logfile not found: $0", logfile_name);
}

RefPtr<msg::MessageSchema> LogfileService::getSchema(
    const LogfileDefinition& cfg) {
  Vector<msg::MessageSchemaField> fields;

  fields.emplace_back(
      msg::MessageSchemaField(
          1,
          "raw",
          msg::FieldType::STRING,
          0,
          false,
          true));

  for (const auto& field : cfg.source_fields()) {
    fields.emplace_back(
        msg::MessageSchemaField(
            field.id(),
            field.name(),
            msg::fieldTypeFromString(field.type()),
            0,
            false,
            true));
  }

  for (const auto& field : cfg.row_fields()) {
    fields.emplace_back(
        msg::MessageSchemaField(
            field.id(),
            field.name(),
            msg::fieldTypeFromString(field.type()),
            0,
            false,
            true));
  }

  return new msg::MessageSchema(cfg.name(), fields);
}

Vector<TableDefinition> LogfileService::getTableDefinitions(
    const CustomerConfig& cfg) {
  Vector<TableDefinition> tbls;

  if (!cfg.has_logfile_import_config()) {
    return tbls;
  }

  for (const auto& logfile : cfg.logfile_import_config().logfiles()) {
    TableDefinition td;
    td.set_customer(cfg.customer());
    td.set_table_name("logs." + logfile.name());
    auto tblcfg = td.mutable_config();
    tblcfg->set_schema(getSchema(logfile)->encode().toString());
    tblcfg->set_partitioner(zbase::TBL_PARTITION_TIMEWINDOW);
    tblcfg->set_storage(zbase::TBL_STORAGE_COLSM);

    auto partcfg = tblcfg->mutable_time_window_partitioner_config();
    partcfg->set_partition_size(10 * kMicrosPerMinute);

    tbls.emplace_back(td);
  }

  return tbls;
}

} // namespace zbase