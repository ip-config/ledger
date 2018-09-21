//------------------------------------------------------------------------------
//
//   Copyright 2018 Fetch.AI Limited
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//------------------------------------------------------------------------------

#include "network/tcp/tcp_server.hpp"

namespace fetch {
namespace network {

TCPServer::TCPServer(uint16_t const &port, network_manager_type const &network_manager)
  : network_manager_{network_manager}
  , port_{port}
  , request_mutex_{}
{
  LOG_STACK_TRACE_POINT;

  FETCH_LOG_INFO(LOGGING_NAME, "Creating TCP server on tcp://0.0.0.0:", port);

  manager_ = std::make_shared<ClientManager>(*this);
}

TCPServer::~TCPServer()
{
  LOG_STACK_TRACE_POINT;
  {
    std::lock_guard<std::mutex> lock(startMutex_);
    stopping_ = true;
  }

  std::weak_ptr<acceptor_type> acceptorWeak = acceptor_;

  network_manager_.Post([acceptorWeak] {
    auto acceptorStrong = acceptorWeak.lock();
    if (acceptorStrong)
    {
      std::error_code dummy;
      acceptorStrong->close(dummy);
      FETCH_LOG_INFO(LOGGING_NAME, "closed TCP server server acceptor: ");
    }
    else
    {
      FETCH_LOG_INFO(LOGGING_NAME, "failed to close acceptor: ");
    }
  });

  while (destruct_guard_.use_count() > 1)
  {
    FETCH_LOG_INFO(LOGGING_NAME, "Waiting for TCP server ", this, " start closure to clear");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  while (!acceptor_.expired() && running_)
  {
    FETCH_LOG_INFO(LOGGING_NAME, "Waiting for TCP server ", this, " to destruct");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  FETCH_LOG_INFO(LOGGING_NAME, "Destructing TCP server ", this);
}

void TCPServer::Start()
{
  std::shared_ptr<int> destruct_guard = destruct_guard_;

  auto closure = [this, destruct_guard] {
    std::lock_guard<std::mutex> lock(startMutex_);

    if (!stopping_)
    {
      std::shared_ptr<acceptor_type> acceptor;

      try
      {  // KLL this also appears to generate a data race.
        // This might throw if the port is not free
        acceptor = network_manager_.CreateIO<acceptor_type>(
            asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port_));

        acceptor_ = acceptor;

        FETCH_LOG_DEBUG(LOGGING_NAME, "Starting TCP server acceptor loop");
        acceptor_ = acceptor;

        if (acceptor)
        {
          running_ = true;
          Accept(acceptor);
          FETCH_LOG_DEBUG(LOGGING_NAME, "Accepting TCP server connections");
        }
      }
      catch (std::exception &e)
      {
        FETCH_LOG_INFO(LOGGING_NAME, "Failed to open socket: ", port_, " with error: ", e.what());
      }
    }
  };

  network_manager_.Post(closure);
}

void TCPServer::Stop()
{}

void TCPServer::PushRequest(connection_handle_type client, message_type const &msg)
{
  LOG_STACK_TRACE_POINT;
  FETCH_LOG_DEBUG(LOGGING_NAME, "Got request from ", client);

  std::lock_guard<mutex_type> lock(request_mutex_);
  requests_.push_back({client, msg});
}

void TCPServer::Broadcast(message_type const &msg)
{
  LOG_STACK_TRACE_POINT;
  manager_->Broadcast(msg);
}

bool TCPServer::Send(connection_handle_type const &client, message_type const &msg)
{
  LOG_STACK_TRACE_POINT;
  return manager_->Send(client, msg);
}

bool TCPServer::has_requests()
{
  LOG_STACK_TRACE_POINT;
  std::lock_guard<mutex_type> lock(request_mutex_);
  bool                        ret = (requests_.size() != 0);
  return ret;
}

TCPServer::Request TCPServer::Top()
{
  std::lock_guard<mutex_type> lock(request_mutex_);
  Request                     top = requests_.front();
  return top;
}

void TCPServer::Pop()
{
  LOG_STACK_TRACE_POINT;
  std::lock_guard<mutex_type> lock(request_mutex_);
  requests_.pop_front();
}

std::string TCPServer::GetAddress(connection_handle_type const &client)
{
  LOG_STACK_TRACE_POINT;
  return manager_->GetAddress(client);
}

void TCPServer::Accept(std::shared_ptr<asio::ip::tcp::tcp::acceptor> acceptor)
{
  LOG_STACK_TRACE_POINT;

  auto strongSocket                = network_manager_.CreateIO<asio::ip::tcp::tcp::socket>();
  std::weak_ptr<ClientManager> man = manager_;

  auto cb = [this, man, acceptor, strongSocket](std::error_code ec) {
    auto lock_ptr = man.lock();
    if (!lock_ptr)
    {
      return;
    }

    if (!ec)
    {
      auto conn = std::make_shared<ClientConnection>(strongSocket, manager_);
      auto ptr  = connection_register_.lock();

      if (ptr)
      {
        ptr->Enter(conn->connection_pointer());
        conn->SetConnectionManager(ptr);
      }

      conn->Start();
      Accept(acceptor);
    }
    else
    {
      FETCH_LOG_INFO(LOGGING_NAME, "Acceptor in TCP server received EC");
    }
  };

  acceptor->async_accept(*strongSocket, cb);
}

}  // namespace network
}  // namespace fetch