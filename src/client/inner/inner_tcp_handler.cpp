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

#include "client/inner/inner_tcp_handler.h"

#include <algorithm>
#include <string>

#include <common/application/application.h>  // for fApp
#include <common/libev/io_loop.h>            // for IoLoop
#include <common/net/net.h>                  // for connect
#include <common/system_info/cpu_info.h>     // for CurrentCpuInfo
#include <common/system_info/system_info.h>  // for AmountOfAvailable...

#include "client/bandwidth/tcp_bandwidth_client.h"  // for TcpBandwidthClient
#include "client/commands.h"
#include "client/events/network_events.h"  // for BandwidtInfo, Con...

#include "inner/inner_client.h"  // for InnerClient

#include "commands_info/channels_info.h"  // for ChannelsInfo
#include "commands_info/client_info.h"    // for ClientInfo
#include "commands_info/ping_info.h"      // for ClientPingInfo
#include "commands_info/runtime_channel_info.h"
#include "commands_info/server_info.h"  // for ServerInfo

namespace fastotv {
namespace client {
namespace inner {

InnerTcpHandler::InnerTcpHandler(const StartConfig& config)
    : fastotv::inner::InnerServerCommandSeqParser(),
      common::libev::IoLoopObserver(),
      inner_connection_(nullptr),
      bandwidth_requests_(),
      ping_server_id_timer_(INVALID_TIMER_ID),
      config_(config),
      current_bandwidth_(0) {}

InnerTcpHandler::~InnerTcpHandler() {
  CHECK(bandwidth_requests_.empty());
  CHECK(!inner_connection_);
}

void InnerTcpHandler::PreLooped(common::libev::IoLoop* server) {
  ping_server_id_timer_ = server->CreateTimer(ping_timeout_server, true);

  Connect(server);
}

void InnerTcpHandler::Accepted(common::libev::IoClient* client) {
  UNUSED(client);
}

void InnerTcpHandler::Moved(common::libev::IoLoop* server, common::libev::IoClient* client) {
  UNUSED(server);
  UNUSED(client);
}

void InnerTcpHandler::Closed(common::libev::IoClient* client) {
  if (client == inner_connection_) {
    fastotv::inner::InnerClient* iclient = static_cast<fastotv::inner::InnerClient*>(client);
    common::net::socket_info info = iclient->GetInfo();
    common::net::HostAndPort host(info.host(), info.port());
    events::ConnectInfo cinf(host);
    fApp->PostEvent(new events::ClientDisconnectedEvent(this, cinf));
    inner_connection_ = nullptr;
    return;
  }

  // bandwidth
  bandwidth::TcpBandwidthClient* band_client = static_cast<bandwidth::TcpBandwidthClient*>(client);
  auto it = std::remove(bandwidth_requests_.begin(), bandwidth_requests_.end(), band_client);
  if (it == bandwidth_requests_.end()) {
    return;
  }

  bandwidth_requests_.erase(it);
  common::net::socket_info info = band_client->GetInfo();
  const common::net::HostAndPort host(info.host(), info.port());
  const BandwidthHostType hs = band_client->GetHostType();
  const bandwidth_t band = band_client->GetDownloadBytesPerSecond();
  if (hs == MAIN_SERVER) {
    current_bandwidth_ = band;
  }
  events::BandwidtInfo cinf(host, band, hs);
  events::BandwidthEstimationEvent* band_event = new events::BandwidthEstimationEvent(this, cinf);
  fApp->PostEvent(band_event);
}

void InnerTcpHandler::DataReceived(common::libev::IoClient* client) {
  if (client == inner_connection_) {
    std::string buff;
    fastotv::inner::InnerClient* iclient = static_cast<fastotv::inner::InnerClient*>(client);
    common::ErrnoError err = iclient->ReadCommand(&buff);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      err = client->Close();
      DCHECK(!err) << "Close client error: " << err->GetDescription();
      delete client;
      return;
    }

    HandleInnerDataReceived(iclient, buff);
    return;
  }

  // bandwidth
  bandwidth::TcpBandwidthClient* band_client = static_cast<bandwidth::TcpBandwidthClient*>(client);
  char buff[bandwidth::TcpBandwidthClient::max_payload_len];
  size_t nwread;
  common::ErrnoError err = band_client->Read(buff, bandwidth::TcpBandwidthClient::max_payload_len, &nwread);
  if (err) {
    int e_code = err->GetErrorCode();
    if (e_code != EINTR) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    err = client->Close();
    DCHECK(!err) << "Close client error: " << err->GetDescription();
    delete client;
  }
}

void InnerTcpHandler::DataReadyToWrite(common::libev::IoClient* client) {
  UNUSED(client);
}

void InnerTcpHandler::PostLooped(common::libev::IoLoop* server) {
  UNUSED(server);
  if (ping_server_id_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(ping_server_id_timer_);
    ping_server_id_timer_ = INVALID_TIMER_ID;
  }
  std::vector<bandwidth::TcpBandwidthClient*> copy = bandwidth_requests_;
  for (bandwidth::TcpBandwidthClient* ban : copy) {
    common::ErrnoError err = ban->Close();
    DCHECK(!err) << "Close client error: " << err->GetDescription();
    delete ban;
  }
  CHECK(bandwidth_requests_.empty());
  DisConnect(common::Error());
  CHECK(!inner_connection_);
}

void InnerTcpHandler::TimerEmited(common::libev::IoLoop* server, common::libev::timer_id_t id) {
  UNUSED(server);
  if (id == ping_server_id_timer_ && inner_connection_) {
    const common::protocols::three_way_handshake::cmd_request_t ping_request = PingRequest(NextRequestID());
    fastotv::inner::InnerClient* client = inner_connection_;
    common::ErrnoError err = client->Write(ping_request);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      err = client->Close();
      DCHECK(!err) << "Close client error: " << err->GetDescription();
      delete client;
    }
  }
}

#if LIBEV_CHILD_ENABLE
void InnerTcpHandler::Accepted(common::libev::IoChild* child) {
  UNUSED(child);
}

void InnerTcpHandler::Moved(common::libev::IoLoop* server, common::libev::IoChild* child) {
  UNUSED(server);
  UNUSED(child);
}

void InnerTcpHandler::ChildStatusChanged(common::libev::IoChild* child, int status) {
  UNUSED(child);
  UNUSED(status);
}
#endif

void InnerTcpHandler::RequestServerInfo() {
  if (!inner_connection_) {
    return;
  }

  const common::protocols::three_way_handshake::cmd_request_t channels_request = GetServerInfoRequest(NextRequestID());
  fastotv::inner::InnerClient* client = inner_connection_;
  common::ErrnoError err = client->Write(channels_request);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    err = client->Close();
    DCHECK(!err) << "Close client error: " << err->GetDescription();
    delete client;
  }
}

void InnerTcpHandler::RequestChannels() {
  if (!inner_connection_) {
    return;
  }

  const common::protocols::three_way_handshake::cmd_request_t channels_request = GetChannelsRequest(NextRequestID());
  fastotv::inner::InnerClient* client = inner_connection_;
  common::ErrnoError err = client->Write(channels_request);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    err = client->Close();
    DCHECK(!err) << "Close client error: " << err->GetDescription();
    delete client;
  }
}

void InnerTcpHandler::PostMessageToChat(const ChatMessage& msg) {
  if (!inner_connection_) {
    return;
  }

  serializet_t msg_ser;
  common::Error err_ser = msg.SerializeToString(&msg_ser);
  if (err_ser) {
    DEBUG_MSG_ERROR(err_ser, common::logging::LOG_LEVEL_ERR);
    return;
  }

  const common::protocols::three_way_handshake::cmd_request_t channels_request =
      SendChatMessageRequest(NextRequestID(), msg_ser);
  fastotv::inner::InnerClient* client = inner_connection_;
  common::ErrnoError err = client->Write(channels_request);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    err = client->Close();
    DCHECK(!err) << "Close client error: " << err->GetDescription();
    delete client;
  }
}

void InnerTcpHandler::RequesRuntimeChannelInfo(stream_id sid) {
  if (!inner_connection_) {
    return;
  }

  const common::protocols::three_way_handshake::cmd_request_t channels_request =
      GetRuntimeChannelInfoRequest(NextRequestID(), sid);
  fastotv::inner::InnerClient* client = inner_connection_;
  common::ErrnoError err = client->Write(channels_request);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    err = client->Close();
    DCHECK(!err) << "Close client error: " << err->GetDescription();
    delete client;
  }
}

void InnerTcpHandler::Connect(common::libev::IoLoop* server) {
  if (!server) {
    return;
  }

  DisConnect(common::make_error("Reconnect"));

  common::net::HostAndPort host = config_.inner_host;
  common::net::socket_info client_info;
  common::ErrnoError err = common::net::connect(host, common::net::ST_SOCK_STREAM, nullptr, &client_info);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    events::ConnectInfo cinf(host);
    auto ex_event =
        common::make_exception_event(new events::ClientConnectedEvent(this, cinf), common::make_error_from_errno(err));
    fApp->PostEvent(ex_event);
    return;
  }

  fastotv::inner::InnerClient* connection = new fastotv::inner::InnerClient(server, client_info);
  inner_connection_ = connection;
  server->RegisterClient(connection);
}

void InnerTcpHandler::DisConnect(common::Error err) {
  UNUSED(err);
  if (inner_connection_) {
    fastotv::inner::InnerClient* connection = inner_connection_;
    common::ErrnoError errn = connection->Close();
    DCHECK(!errn) << "Close connection error: " << errn->GetDescription();
    delete connection;
  }
}

common::ErrnoError InnerTcpHandler::CreateAndConnectTcpBandwidthClient(common::libev::IoLoop* server,
                                                                       const common::net::HostAndPort& host,
                                                                       BandwidthHostType hs,
                                                                       bandwidth::TcpBandwidthClient** out_band) {
  if (!server || !out_band) {
    return common::make_errno_error_inval();
  }

  common::net::socket_info client_info;
  common::ErrnoError err = common::net::connect(host, common::net::ST_SOCK_STREAM, nullptr, &client_info);
  if (err) {
    return err;
  }

  bandwidth::TcpBandwidthClient* connection = new bandwidth::TcpBandwidthClient(server, client_info, hs);
  err = connection->StartSession(0, 1000);
  if (err) {
    common::ErrnoError err_close = connection->Close();
    DCHECK(!err_close) << "Close connection error: " << err_close->GetDescription();
    delete connection;
    return err;
  }

  *out_band = connection;
  return common::ErrnoError();
}

void InnerTcpHandler::HandleInnerRequestCommand(fastotv::inner::InnerClient* connection,
                                                common::protocols::three_way_handshake::cmd_seq_t id,
                                                int argc,
                                                char* argv[]) {
  UNUSED(argc);
  char* command = argv[0];

  if (IS_EQUAL_COMMAND(command, SERVER_PING)) {
    ServerPingInfo ping;
    json_object* jping = nullptr;
    common::Error err_ser = ping.Serialize(&jping);
    CHECK(!err_ser) << "Serialize error: " << err_ser->GetDescription();
    std::string ping_str = json_object_get_string(jping);
    json_object_put(jping);
    const common::protocols::three_way_handshake::cmd_response_t pong = PingResponceSuccsess(id, ping_str);
    common::ErrnoError err = connection->Write(pong);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    return;
  } else if (IS_EQUAL_COMMAND(command, SERVER_WHO_ARE_YOU)) {
    json_object* jauth = nullptr;
    common::Error err_ser = config_.ainf.Serialize(&jauth);
    if (err_ser) {
      DEBUG_MSG_ERROR(err_ser, common::logging::LOG_LEVEL_ERR);
      return;
    }

    std::string auth_str = json_object_get_string(jauth);
    json_object_put(jauth);
    common::protocols::three_way_handshake::cmd_response_t iAm = WhoAreYouResponceSuccsess(id, auth_str);
    common::ErrnoError err = connection->Write(iAm);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    return;
  } else if (IS_EQUAL_COMMAND(command, SERVER_GET_CLIENT_INFO)) {
    const common::system_info::CpuInfo& c1 = common::system_info::CurrentCpuInfo();
    std::string brand = c1.GetBrandName();

    int64_t ram_total = common::system_info::AmountOfPhysicalMemory();
    int64_t ram_free = common::system_info::AmountOfAvailablePhysicalMemory();

    std::string os_name = common::system_info::OperatingSystemName();
    std::string os_version = common::system_info::OperatingSystemVersion();
    std::string os_arch = common::system_info::OperatingSystemArchitecture();

    std::string os = common::MemSPrintf("%s %s(%s)", os_name, os_version, os_arch);

    ClientInfo info(config_.ainf.GetLogin(), os, brand, ram_total, ram_free, current_bandwidth_);
    serializet_t info_json_string;
    common::Error err_ser = info.SerializeToString(&info_json_string);
    if (err_ser) {
      DEBUG_MSG_ERROR(err_ser, common::logging::LOG_LEVEL_ERR);
      return;
    }

    common::protocols::three_way_handshake::cmd_response_t resp = SystemInfoResponceSuccsess(id, info_json_string);
    common::ErrnoError err = connection->Write(resp);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    return;
  } else if (IS_EQUAL_COMMAND(command, SERVER_SEND_CHAT_MESSAGE)) {
    if (argc < 2 || !argv[1]) {
      common::Error parse_err = common::make_error_inval();
      DEBUG_MSG_ERROR(parse_err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    json_object* jmsg = json_tokener_parse(argv[1]);
    if (!jmsg) {
      common::Error parse_err = common::make_error_inval();
      DEBUG_MSG_ERROR(parse_err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    ChatMessage msg;
    common::Error err_ser = msg.DeSerialize(jmsg);
    std::string msg_str = json_object_get_string(jmsg);
    json_object_put(jmsg);
    if (err_ser) {
      return;
    }

    fApp->PostEvent(new events::ReceiveChatMessageEvent(this, msg));
    common::protocols::three_way_handshake::cmd_response_t resp = SystemInfoResponceSuccsess(id, msg_str);
    common::ErrnoError err = connection->Write(resp);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    return;
  }

  WARNING_LOG() << "UNKNOWN REQUEST COMMAND: " << command;
}

void InnerTcpHandler::HandleInnerResponceCommand(fastotv::inner::InnerClient* connection,
                                                 common::protocols::three_way_handshake::cmd_seq_t id,
                                                 int argc,
                                                 char* argv[]) {
  char* state_command = argv[0];

  if (IS_EQUAL_COMMAND(state_command, SUCCESS_COMMAND) && argc > 1) {
    common::ErrnoError err = HandleInnerSuccsessResponceCommand(connection, id, argc, argv);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    return;
  } else if (IS_EQUAL_COMMAND(state_command, FAIL_COMMAND) && argc > 1) {
    common::ErrnoError err = HandleInnerFailedResponceCommand(connection, id, argc, argv);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    return;
  }

  WARNING_LOG() << "UNKNOWN STATE COMMAND: " << state_command;
}

void InnerTcpHandler::HandleInnerApproveCommand(fastotv::inner::InnerClient* connection,
                                                common::protocols::three_way_handshake::cmd_seq_t id,
                                                int argc,
                                                char* argv[]) {
  UNUSED(id);
  char* command = argv[0];

  if (IS_EQUAL_COMMAND(command, SUCCESS_COMMAND)) {
    if (argc > 1) {
      const char* okrespcommand = argv[1];
      if (IS_EQUAL_COMMAND(okrespcommand, SERVER_PING)) {
      } else if (IS_EQUAL_COMMAND(okrespcommand, SERVER_WHO_ARE_YOU)) {
        connection->SetName(config_.ainf.GetLogin());
        fApp->PostEvent(new events::ClientAuthorizedEvent(this, config_.ainf));
      } else if (IS_EQUAL_COMMAND(okrespcommand, SERVER_GET_CLIENT_INFO)) {
      } else if (IS_EQUAL_COMMAND(okrespcommand, SERVER_SEND_CHAT_MESSAGE)) {
      }
    }
    return;
  } else if (IS_EQUAL_COMMAND(command, FAIL_COMMAND)) {
    if (argc > 1) {
      const char* failed_resp_command = argv[1];
      if (IS_EQUAL_COMMAND(failed_resp_command, SERVER_PING)) {
      } else if (IS_EQUAL_COMMAND(failed_resp_command, SERVER_WHO_ARE_YOU)) {
        common::Error err = common::make_error(argc > 2 ? argv[2] : "Unknown");
        auto ex_event = common::make_exception_event(new events::ClientAuthorizedEvent(this, config_.ainf), err);
        fApp->PostEvent(ex_event);
      } else if (IS_EQUAL_COMMAND(failed_resp_command, SERVER_GET_CLIENT_INFO)) {
      } else if (IS_EQUAL_COMMAND(failed_resp_command, SERVER_SEND_CHAT_MESSAGE)) {
      }
    }
    return;
  }

  WARNING_LOG() << "UNKNOWN COMMAND: " << command;
}

common::ErrnoError InnerTcpHandler::HandleInnerSuccsessResponceCommand(
    fastotv::inner::InnerClient* connection,
    common::protocols::three_way_handshake::cmd_seq_t id,
    int argc,
    char* argv[]) {
  char* command = argv[1];
  if (IS_EQUAL_COMMAND(command, CLIENT_PING)) {
    json_object* obj = nullptr;
    common::Error parse_err = ParserResponceResponceCommand(argc, argv, &obj);
    if (parse_err) {
      common::protocols::three_way_handshake::cmd_approve_t resp =
          PingApproveResponceFail(id, parse_err->GetDescription());
      ignore_result(connection->Write(resp));
      return common::make_errno_error(parse_err->GetDescription(), EINVAL);
    }

    ClientPingInfo ping_info;
    common::Error err = ping_info.DeSerialize(obj);
    json_object_put(obj);
    if (err) {
      return common::make_errno_error(err->GetDescription(), EINVAL);
    }
    common::protocols::three_way_handshake::cmd_approve_t resp = PingApproveResponceSuccsess(id);
    return connection->Write(resp);
  } else if (IS_EQUAL_COMMAND(command, CLIENT_GET_SERVER_INFO)) {
    json_object* obj = nullptr;
    common::Error parse_err = ParserResponceResponceCommand(argc, argv, &obj);
    if (parse_err) {
      common::protocols::three_way_handshake::cmd_approve_t resp =
          GetServerInfoApproveResponceFail(id, parse_err->GetDescription());
      ignore_result(connection->Write(resp));
      return common::make_errno_error(parse_err->GetDescription(), EINVAL);
    }

    ServerInfo sinf;
    common::Error err = sinf.DeSerialize(obj);
    json_object_put(obj);
    if (err) {
      return common::make_errno_error(err->GetDescription(), EINVAL);
    }

    common::net::HostAndPort host = sinf.GetBandwidthHost();
    bandwidth::TcpBandwidthClient* band_connection = nullptr;
    common::libev::IoLoop* server = connection->GetServer();
    const BandwidthHostType hs = MAIN_SERVER;
    common::ErrnoError errn = CreateAndConnectTcpBandwidthClient(server, host, hs, &band_connection);
    if (errn) {
      events::BandwidtInfo cinf(host, 0, hs);
      current_bandwidth_ = 0;
      auto ex_event = common::make_exception_event(new events::BandwidthEstimationEvent(this, cinf),
                                                   common::make_error_from_errno(errn));
      fApp->PostEvent(ex_event);
      return errn;
    }

    bandwidth_requests_.push_back(band_connection);
    server->RegisterClient(band_connection);
    return common::ErrnoError();
  } else if (IS_EQUAL_COMMAND(command, CLIENT_GET_CHANNELS)) {
    json_object* obj = nullptr;
    common::Error parse_err = ParserResponceResponceCommand(argc, argv, &obj);
    if (parse_err) {
      common::protocols::three_way_handshake::cmd_approve_t resp =
          GetChannelsApproveResponceFail(id, parse_err->GetDescription());
      ignore_result(connection->Write(resp));
      return common::make_errno_error(parse_err->GetDescription(), EINVAL);
    }

    ChannelsInfo chan;
    common::Error err = chan.DeSerialize(obj);
    json_object_put(obj);
    if (err) {
      return common::make_errno_error(err->GetDescription(), EINVAL);
    }

    fApp->PostEvent(new events::ReceiveChannelsEvent(this, chan));
    const common::protocols::three_way_handshake::cmd_approve_t resp = GetChannelsApproveResponceSuccsess(id);
    return connection->Write(resp);
  } else if (IS_EQUAL_COMMAND(command, CLIENT_GET_RUNTIME_CHANNEL_INFO)) {
    json_object* obj = nullptr;
    common::Error parse_err = ParserResponceResponceCommand(argc, argv, &obj);
    if (parse_err) {
      common::protocols::three_way_handshake::cmd_approve_t resp =
          GetRuntimeChannelInfoApproveResponceFail(id, parse_err->GetDescription());
      ignore_result(connection->Write(resp));
      return common::make_errno_error(parse_err->GetDescription(), EINVAL);
    }

    RuntimeChannelInfo chan;
    common::Error err = chan.DeSerialize(obj);
    json_object_put(obj);
    if (err) {
      return common::make_errno_error(err->GetDescription(), EINVAL);
    }

    fApp->PostEvent(new events::ReceiveRuntimeChannelEvent(this, chan));
    const common::protocols::three_way_handshake::cmd_approve_t resp = GetRuntimeChannelInfoApproveResponceSuccsess(id);
    return connection->Write(resp);
  } else if (IS_EQUAL_COMMAND(command, CLIENT_SEND_CHAT_MESSAGE)) {
    json_object* obj = nullptr;
    common::Error parse_err = ParserResponceResponceCommand(argc, argv, &obj);
    if (parse_err) {
      common::protocols::three_way_handshake::cmd_approve_t resp =
          SendChatMessageApproveResponceFail(id, parse_err->GetDescription());
      ignore_result(connection->Write(resp));
      return common::make_errno_error(parse_err->GetDescription(), EINVAL);
    }

    ChatMessage msg;
    common::Error err = msg.DeSerialize(obj);
    json_object_put(obj);
    if (err) {
      return common::make_errno_error(err->GetDescription(), EINVAL);
    }

    fApp->PostEvent(new events::SendChatMessageEvent(this, msg));
    const common::protocols::three_way_handshake::cmd_approve_t resp = SendChatMessageApproveResponceSuccsess(id);
    return connection->Write(resp);
  }

  const std::string error_str = common::MemSPrintf("UNKNOWN RESPONCE COMMAND: %s", command);
  return common::make_errno_error(error_str, EINVAL);
}

common::ErrnoError InnerTcpHandler::HandleInnerFailedResponceCommand(
    fastotv::inner::InnerClient* connection,
    common::protocols::three_way_handshake::cmd_seq_t id,
    int argc,
    char* argv[]) {
  UNUSED(connection);
  UNUSED(id);
  UNUSED(argc);

  char* command = argv[1];
  const std::string error_str =
      common::MemSPrintf("Sorry now we can't handle failed pesponce for command: %s", command);
  return common::make_errno_error(error_str, EINVAL);
}

common::Error InnerTcpHandler::ParserResponceResponceCommand(int argc, char* argv[], json_object** out) {
  if (argc < 2) {
    return common::make_error_inval();
  }

  const char* arg_2_str = argv[2];
  if (!arg_2_str) {
    return common::make_error_inval();
  }

  json_object* obj = json_tokener_parse(arg_2_str);
  if (!obj) {
    return common::make_error_inval();
  }

  *out = obj;
  return common::Error();
}

}  // namespace inner
}  // namespace client
}  // namespace fastotv
