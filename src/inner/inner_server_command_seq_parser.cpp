/*  Copyright (C) 2014-2018 FastoGT. All right reserved.

    This file is part of FastoTV.

    FastoTV is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    FastoTV is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with FastoTV. If not, see <http://www.gnu.org/licenses/>.
*/

#include "inner/inner_server_command_seq_parser.h"

#include <algorithm>
#include <string>

#include <common/convert2string.h>
#include <common/sys_byteorder.h>

#include "inner/inner_client.h"  // for InnerClient

extern "C" {
#include "sds_fasto.h"  // for sdsfreesplitres, sds
}

#define GB (1024 * 1024 * 1024)
#define BUF_SIZE 4096

namespace fastotv {
namespace inner {

RequestCallback::RequestCallback(common::protocols::three_way_handshake::cmd_seq_t request_id, callback_t cb)
    : request_id_(request_id), cb_(cb) {}

common::protocols::three_way_handshake::cmd_seq_t RequestCallback::GetRequestID() const {
  return request_id_;
}

void RequestCallback::Execute(int argc, char* argv[]) {
  if (!cb_) {
    return;
  }

  return cb_(request_id_, argc, argv);
}

InnerServerCommandSeqParser::InnerServerCommandSeqParser() : id_() {}

InnerServerCommandSeqParser::~InnerServerCommandSeqParser() {}

common::protocols::three_way_handshake::cmd_seq_t InnerServerCommandSeqParser::NextRequestID() {
  const seq_id_t next_id = id_++;
  char bytes[sizeof(seq_id_t)];
  const seq_id_t stabled = common::NetToHost64(next_id);  // for human readable hex
  memcpy(&bytes, &stabled, sizeof(seq_id_t));
  common::protocols::three_way_handshake::cmd_seq_t hexed;
  common::utils::hex::encode(std::string(bytes, sizeof(seq_id_t)), true, &hexed);
  return hexed;
}

namespace {

bool exec_reqest(RequestCallback req,
                 common::protocols::three_way_handshake::cmd_seq_t request_id,
                 int argc,
                 char* argv[]) {
  if (request_id == req.GetRequestID()) {
    req.Execute(argc, argv);
    return true;
  }

  return false;
}

}  // namespace

void InnerServerCommandSeqParser::ProcessRequest(common::protocols::three_way_handshake::cmd_seq_t request_id,
                                                 int argc,
                                                 char* argv[]) {
  subscribed_requests_.erase(std::remove_if(subscribed_requests_.begin(), subscribed_requests_.end(),
                                            std::bind(&exec_reqest, std::placeholders::_1, request_id, argc, argv)),
                             subscribed_requests_.end());
}

void InnerServerCommandSeqParser::SubscribeRequest(const RequestCallback& req) {
  subscribed_requests_.push_back(req);
}

void InnerServerCommandSeqParser::HandleInnerDataReceived(InnerClient* connection, const std::string& input_command) {
  common::protocols::three_way_handshake::cmd_id_t seq;
  common::protocols::three_way_handshake::cmd_seq_t id;
  std::string cmd_str;

  common::Error err = common::protocols::three_way_handshake::ParseCommand(input_command, &seq, &id, &cmd_str);
  if (err) {
    WARNING_LOG() << err->GetDescription();
    common::ErrnoError errn = connection->Close();
    DCHECK(!errn);
    delete connection;
    return;
  }

  int argc;
  sds* argv = sdssplitargslong(cmd_str.c_str(), &argc);
  if (argv == nullptr) {
    const std::string error_str = "PROBLEM PARSING INNER COMMAND: " + input_command;
    WARNING_LOG() << error_str;
    common::ErrnoError errn = connection->Close();
    DCHECK(!errn);
    delete connection;
    return;
  }

  ProcessRequest(id, argc, argv);
  INFO_LOG() << "HANDLE INNER COMMAND client[" << connection->GetFormatedName()
             << "] seq: " << common::protocols::three_way_handshake::CmdIdToString(seq) << ", id:" << id
             << ", cmd: " << cmd_str;
  if (seq == REQUEST_COMMAND) {
    HandleInnerRequestCommand(connection, id, argc, argv);
  } else if (seq == RESPONSE_COMMAND) {
    HandleInnerResponceCommand(connection, id, argc, argv);
  } else if (seq == APPROVE_COMMAND) {
    HandleInnerApproveCommand(connection, id, argc, argv);
  } else {
    DNOTREACHED();
    common::ErrnoError errn = connection->Close();
    DCHECK(!errn);
    delete connection;
  }
  sdsfreesplitres(argv, argc);
}

}  // namespace inner
}  // namespace fastotv
