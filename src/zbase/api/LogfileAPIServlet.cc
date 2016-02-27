/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "stx/wallclock.h"
#include "stx/assets.h"
#include "stx/protobuf/msg.h"
#include "stx/io/BufferedOutputStream.h"
#include "zbase/api/LogfileAPIServlet.h"

using namespace stx;

namespace zbase {

LogfileAPIServlet::LogfileAPIServlet(
    LogfileService* service,
    ConfigDirectory* cdir,
    const String& cachedir) :
    service_(service),
    cdir_(cdir),
    cachedir_(cachedir) {}

void LogfileAPIServlet::handle(
    const AnalyticsSession& session,
    RefPtr<stx::http::HTTPRequestStream> req_stream,
    RefPtr<stx::http::HTTPResponseStream> res_stream) {
  const auto& req = req_stream->request();
  URI uri(req.uri());

  http::HTTPResponse res;
  res.populateFromRequest(req);

  if (uri.path() == "/api/v1/logfiles") {
    req_stream->readBody();
    listLogfiles(session, uri, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/logfiles/get_definition") {
    req_stream->readBody();
    fetchLogfileDefinition(session, uri, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/logfiles/set_regex") {
    req_stream->readBody();
    setLogfileRegex(session, uri, &req, &res);
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/logfiles/upload") {
    uploadLogfile(session, uri, req_stream.get(), &res);
    res_stream->writeResponse(res);
    return;
  }

  res.setStatus(http::kStatusNotFound);
  res.addHeader("Content-Type", "text/html; charset=utf-8");
  res.addBody(Assets::getAsset("zbase/webui/404.html"));
  res_stream->writeResponse(res);
}

void LogfileAPIServlet::listLogfiles(
    const AnalyticsSession& session,
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  auto customer_conf = cdir_->configFor(session.customer());
  const auto& logfile_cfg = customer_conf->config.logfile_import_config();

  Buffer buf;
  json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
  json.beginObject();
  json.addObjectEntry("logfile_definitions");
  json.beginArray();

  size_t nlogs = 0;
  for (const auto& logfile : logfile_cfg.logfiles()) {
    if (++nlogs > 1) {
      json.addComma();
    }

    renderLogfileDefinition(&logfile, &json);
  }

  json.endArray();
  json.endObject();

  res->setStatus(http::kStatusOK);
  res->setHeader("Content-Type", "application/json; charset=utf-8");
  res->addBody(buf);
}

void LogfileAPIServlet::fetchLogfileDefinition(
    const AnalyticsSession& session,
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  const auto& params = uri.queryParams();
  auto customer_conf = cdir_->configFor(session.customer());
  const auto& logfile_cfg = customer_conf->config.logfile_import_config();

  String logfile_name;
  if (!URI::getParam(params, "logfile", &logfile_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?logfile=... parameter");
    return;
  }

  const LogfileDefinition* logfile_def = nullptr;
  for (const auto& logfile : logfile_cfg.logfiles()) {
    if (logfile.name() == logfile_name) {
      logfile_def = &logfile;
      break;
    }
  }

  if (logfile_def == nullptr) {
    res->setStatus(http::kStatusNotFound);
    res->addBody("logfile not found");
  } else {
    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
    renderLogfileDefinition(logfile_def, &json);
    res->setStatus(http::kStatusOK);
    res->setHeader("Content-Type", "application/json; charset=utf-8");
    res->addBody(buf);
  }
}

void LogfileAPIServlet::setLogfileRegex(
    const AnalyticsSession& session,
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  const auto& params = uri.queryParams();
  auto customer_conf = cdir_->configFor(session.customer());

  String logfile_name;
  if (!URI::getParam(params, "logfile", &logfile_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?logfile=... parameter");
    return;
  }

  String regex_str;
  if (!URI::getParam(params, "regex", &regex_str)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("error: missing ?regex=... parameter");
    return;
  }

  service_->setLogfileRegex(session.customer(), logfile_name, regex_str);

  res->setStatus(http::kStatusCreated);
  res->addBody("ok");
}

void LogfileAPIServlet::renderLogfileDefinition(
    const LogfileDefinition* logfile_def,
    json::JSONOutputStream* json) {
  json->beginObject();

  json->addObjectEntry("name");
  json->addString(logfile_def->name());
  json->addComma();

  json->addObjectEntry("regex");
  json->addString(logfile_def->regex());
  json->addComma();

  json->addObjectEntry("source_fields");
  json->beginArray();
  {
    size_t nfields = 0;
    for (const auto& field : logfile_def->source_fields()) {
      if (++nfields > 1) {
        json->addComma();
      }

      json->beginObject();

      json->addObjectEntry("name");
      json->addString(field.name());
      json->addComma();

      json->addObjectEntry("id");
      json->addInteger(field.id());
      json->addComma();

      json->addObjectEntry("type");
      json->addString(field.type());
      json->addComma();

      json->addObjectEntry("format");
      json->addString(field.format());

      json->endObject();
    }
  }
  json->endArray();
  json->addComma();

  json->addObjectEntry("row_fields");
  json->beginArray();
  {
    size_t nfields = 0;
    for (const auto& field : logfile_def->row_fields()) {
      if (++nfields > 1) {
        json->addComma();
      }

      json->beginObject();

      json->addObjectEntry("name");
      json->addString(field.name());
      json->addComma();

      json->addObjectEntry("id");
      json->addInteger(field.id());
      json->addComma();

      json->addObjectEntry("type");
      json->addString(field.type());
      json->addComma();

      json->addObjectEntry("format");
      json->addString(field.format());

      json->endObject();
    }
  }
  json->endArray();

  json->endObject();
}

}
