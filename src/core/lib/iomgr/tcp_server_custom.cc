/*
 *
 * Copyright 2018 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"

#include <assert.h>
#include <string.h>

#include <string>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/tcp_custom.h"
#include "src/core/lib/iomgr/tcp_server.h"

extern grpc_core::TraceFlag grpc_tcp_trace;

extern grpc_socket_vtable* grpc_custom_socket_vtable;

/* one listening port */
struct grpc_tcp_listener {
  grpc_tcp_server* server;
  unsigned port_index;
  int port;

  grpc_custom_socket* socket;

  /* linked list */
  struct grpc_tcp_listener* next;

  bool closed;
};

struct grpc_tcp_server {
  gpr_refcount refs;

  /* Called whenever accept() succeeds on a server port. */
  grpc_tcp_server_cb on_accept_cb;
  void* on_accept_cb_arg;

  int open_ports;

  /* linked list of server ports */
  grpc_tcp_listener* head;
  grpc_tcp_listener* tail;

  /* List of closures passed to shutdown_starting_add(). */
  grpc_closure_list shutdown_starting;

  /* shutdown callback */
  grpc_closure* shutdown_complete;

  bool shutdown;
  bool so_reuseport;

  grpc_slice_allocator_factory* slice_allocator_factory;
};

static grpc_error_handle tcp_server_create(
    grpc_closure* shutdown_complete, const grpc_channel_args* args,
    grpc_slice_allocator_factory* slice_allocator_factory,
    grpc_tcp_server** server) {
  grpc_tcp_server* s =
      static_cast<grpc_tcp_server*>(gpr_malloc(sizeof(grpc_tcp_server)));
  s->so_reuseport =
      grpc_channel_args_find_bool(args, GRPC_ARG_ALLOW_REUSEPORT, true);
  gpr_ref_init(&s->refs, 1);
  s->on_accept_cb = nullptr;
  s->on_accept_cb_arg = nullptr;
  s->open_ports = 0;
  s->head = nullptr;
  s->tail = nullptr;
  s->shutdown_starting.head = nullptr;
  s->shutdown_starting.tail = nullptr;
  s->shutdown_complete = shutdown_complete;
  s->shutdown = false;
  s->slice_allocator_factory = slice_allocator_factory;
  *server = s;
  return GRPC_ERROR_NONE;
}

static grpc_tcp_server* tcp_server_ref(grpc_tcp_server* s) {
  GRPC_CUSTOM_IOMGR_ASSERT_SAME_THREAD();
  gpr_ref(&s->refs);
  return s;
}

static void tcp_server_shutdown_starting_add(grpc_tcp_server* s,
                                             grpc_closure* shutdown_starting) {
  grpc_closure_list_append(&s->shutdown_starting, shutdown_starting,
                           GRPC_ERROR_NONE);
}

static void finish_shutdown(grpc_tcp_server* s) {
  GPR_ASSERT(s->shutdown);
  if (s->shutdown_complete != nullptr) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, s->shutdown_complete,
                            GRPC_ERROR_NONE);
  }

  while (s->head) {
    grpc_tcp_listener* sp = s->head;
    s->head = sp->next;
    sp->next = nullptr;
    gpr_free(sp);
  }
  grpc_slice_allocator_factory_destroy(s->slice_allocator_factory);
  gpr_free(s);
}

static void custom_close_callback(grpc_custom_socket* socket) {
  grpc_tcp_listener* sp = socket->listener;
  if (sp) {
    grpc_core::ExecCtx exec_ctx;
    sp->server->open_ports--;
    if (sp->server->open_ports == 0 && sp->server->shutdown) {
      finish_shutdown(sp->server);
    }
  }
  socket->refs--;
  if (socket->refs == 0) {
    grpc_custom_socket_vtable->destroy(socket);
    gpr_free(socket);
  }
}

void grpc_custom_close_server_callback(grpc_tcp_listener* listener) {
  if (listener) {
    grpc_core::ExecCtx exec_ctx;
    listener->server->open_ports--;
    if (listener->server->open_ports == 0 && listener->server->shutdown) {
      finish_shutdown(listener->server);
    }
  }
}

static void close_listener(grpc_tcp_listener* sp) {
  grpc_custom_socket* socket = sp->socket;
  if (!sp->closed) {
    sp->closed = true;
    grpc_custom_socket_vtable->close(socket, custom_close_callback);
  }
}

static void tcp_server_destroy(grpc_tcp_server* s) {
  int immediately_done = 0;
  grpc_tcp_listener* sp;

  GPR_ASSERT(!s->shutdown);
  s->shutdown = true;

  if (s->open_ports == 0) {
    immediately_done = 1;
  }
  for (sp = s->head; sp; sp = sp->next) {
    close_listener(sp);
  }

  if (immediately_done) {
    finish_shutdown(s);
  }
}

static void tcp_server_unref(grpc_tcp_server* s) {
  GRPC_CUSTOM_IOMGR_ASSERT_SAME_THREAD();
  if (gpr_unref(&s->refs)) {
    /* Complete shutdown_starting work before destroying. */
    grpc_core::ExecCtx exec_ctx;
    grpc_core::ExecCtx::RunList(DEBUG_LOCATION, &s->shutdown_starting);
    grpc_core::ExecCtx::Get()->Flush();
    tcp_server_destroy(s);
  }
}

static void finish_accept(grpc_tcp_listener* sp, grpc_custom_socket* socket) {
  grpc_tcp_server_acceptor* acceptor =
      static_cast<grpc_tcp_server_acceptor*>(gpr_malloc(sizeof(*acceptor)));
  grpc_endpoint* ep = nullptr;
  grpc_resolved_address peer_name;
  std::string peer_name_string;
  grpc_error_handle err;

  memset(&peer_name, 0, sizeof(grpc_resolved_address));
  peer_name.len = GRPC_MAX_SOCKADDR_SIZE;
  err = grpc_custom_socket_vtable->getpeername(
      socket, reinterpret_cast<grpc_sockaddr*>(&peer_name.addr),
      reinterpret_cast<int*>(&peer_name.len));
  if (err == GRPC_ERROR_NONE) {
    peer_name_string = grpc_sockaddr_to_uri(&peer_name);
  } else {
    GRPC_LOG_IF_ERROR("getpeername error", err);
    GRPC_ERROR_UNREF(err);
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "SERVER_CONNECT: %p accepted connection: %s", sp->server,
            peer_name_string.c_str());
  }
  ep = custom_tcp_endpoint_create(
      socket,
      grpc_slice_allocator_factory_create_slice_allocator(
          sp->server->slice_allocator_factory, peer_name_string),
      peer_name_string.c_str());
  acceptor->from_server = sp->server;
  acceptor->port_index = sp->port_index;
  acceptor->fd_index = 0;
  acceptor->external_connection = false;
  sp->server->on_accept_cb(sp->server->on_accept_cb_arg, ep, nullptr, acceptor);
}

static void custom_accept_callback(grpc_custom_socket* socket,
                                   grpc_custom_socket* client,
                                   grpc_error_handle error);

static void custom_accept_callback(grpc_custom_socket* socket,
                                   grpc_custom_socket* client,
                                   grpc_error_handle error) {
  grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
  grpc_core::ExecCtx exec_ctx;
  grpc_tcp_listener* sp = socket->listener;
  if (error != GRPC_ERROR_NONE) {
    if (!sp->closed) {
      gpr_log(GPR_ERROR, "Accept failed: %s",
              grpc_error_std_string(error).c_str());
    }
    gpr_free(client);
    GRPC_ERROR_UNREF(error);
    return;
  }
  finish_accept(sp, client);
  if (!sp->closed) {
    grpc_custom_socket* new_socket = static_cast<grpc_custom_socket*>(
        gpr_malloc(sizeof(grpc_custom_socket)));
    new_socket->endpoint = nullptr;
    new_socket->listener = nullptr;
    new_socket->connector = nullptr;
    new_socket->refs = 1;
    grpc_custom_socket_vtable->accept(sp->socket, new_socket,
                                      custom_accept_callback);
  }
}

static grpc_error_handle add_socket_to_server(grpc_tcp_server* s,
                                              grpc_custom_socket* socket,
                                              const grpc_resolved_address* addr,
                                              unsigned port_index,
                                              grpc_tcp_listener** listener) {
  grpc_tcp_listener* sp = nullptr;
  int port = -1;
  grpc_error_handle error;
  grpc_resolved_address sockname_temp;

  // NOTE(lidiz) The last argument is "flags" which is unused by other
  // implementations. Python IO managers uses it to specify SO_REUSEPORT.
  int flags = 0;
  if (s->so_reuseport) {
    flags |= GRPC_CUSTOM_SOCKET_OPT_SO_REUSEPORT;
  }

  error = grpc_custom_socket_vtable->bind(
      socket, reinterpret_cast<grpc_sockaddr*>(const_cast<char*>(addr->addr)),
      addr->len, flags);
  if (error != GRPC_ERROR_NONE) {
    return error;
  }

  error = grpc_custom_socket_vtable->listen(socket);
  if (error != GRPC_ERROR_NONE) {
    return error;
  }

  sockname_temp.len = GRPC_MAX_SOCKADDR_SIZE;
  error = grpc_custom_socket_vtable->getsockname(
      socket, reinterpret_cast<grpc_sockaddr*>(&sockname_temp.addr),
      reinterpret_cast<int*>(&sockname_temp.len));
  if (error != GRPC_ERROR_NONE) {
    return error;
  }

  port = grpc_sockaddr_get_port(&sockname_temp);

  GPR_ASSERT(port >= 0);
  GPR_ASSERT(!s->on_accept_cb && "must add ports before starting server");
  sp = static_cast<grpc_tcp_listener*>(gpr_zalloc(sizeof(grpc_tcp_listener)));
  sp->next = nullptr;
  if (s->head == nullptr) {
    s->head = sp;
  } else {
    s->tail->next = sp;
  }
  s->tail = sp;
  sp->server = s;
  sp->socket = socket;
  sp->port = port;
  sp->port_index = port_index;
  sp->closed = false;
  s->open_ports++;
  *listener = sp;

  return GRPC_ERROR_NONE;
}

static grpc_error_handle tcp_server_add_port(grpc_tcp_server* s,
                                             const grpc_resolved_address* addr,
                                             int* port) {
  // This function is mostly copied from tcp_server_windows.c
  grpc_tcp_listener* sp = nullptr;
  grpc_custom_socket* socket;
  grpc_resolved_address addr6_v4mapped;
  grpc_resolved_address wildcard;
  grpc_resolved_address* allocated_addr = nullptr;
  grpc_resolved_address sockname_temp;
  unsigned port_index = 0;
  grpc_error_handle error = GRPC_ERROR_NONE;
  int family;

  GRPC_CUSTOM_IOMGR_ASSERT_SAME_THREAD();

  if (s->tail != nullptr) {
    port_index = s->tail->port_index + 1;
  }

  /* Check if this is a wildcard port, and if so, try to keep the port the same
     as some previously created listener. */
  if (grpc_sockaddr_get_port(addr) == 0) {
    for (sp = s->head; sp; sp = sp->next) {
      socket = sp->socket;
      sockname_temp.len = GRPC_MAX_SOCKADDR_SIZE;
      if (grpc_custom_socket_vtable->getsockname(
              socket, reinterpret_cast<grpc_sockaddr*>(&sockname_temp.addr),
              reinterpret_cast<int*>(&sockname_temp.len)) == GRPC_ERROR_NONE) {
        *port = grpc_sockaddr_get_port(&sockname_temp);
        if (*port > 0) {
          allocated_addr = static_cast<grpc_resolved_address*>(
              gpr_malloc(sizeof(grpc_resolved_address)));
          memcpy(allocated_addr, addr, sizeof(grpc_resolved_address));
          grpc_sockaddr_set_port(allocated_addr, *port);
          addr = allocated_addr;
          break;
        }
      }
    }
  }

  if (grpc_sockaddr_to_v4mapped(addr, &addr6_v4mapped)) {
    addr = &addr6_v4mapped;
  }

  /* Treat :: or 0.0.0.0 as a family-agnostic wildcard. */
  if (grpc_sockaddr_is_wildcard(addr, port)) {
    grpc_sockaddr_make_wildcard6(*port, &wildcard);

    addr = &wildcard;
  }

  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "SERVER %p add_port %s error=%s", s,
            grpc_sockaddr_to_string(addr, false).c_str(),
            grpc_error_std_string(error).c_str());
  }

  family = grpc_sockaddr_get_family(addr);
  socket =
      static_cast<grpc_custom_socket*>(gpr_malloc(sizeof(grpc_custom_socket)));
  socket->refs = 1;
  socket->endpoint = nullptr;
  socket->listener = nullptr;
  socket->connector = nullptr;
  error = grpc_custom_socket_vtable->init(socket, family);

  if (error == GRPC_ERROR_NONE) {
    error = add_socket_to_server(s, socket, addr, port_index, &sp);
  }
  gpr_free(allocated_addr);

  if (error != GRPC_ERROR_NONE) {
    grpc_error_handle error_out =
        GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
            "Failed to add port to server", &error, 1);
    GRPC_ERROR_UNREF(error);
    error = error_out;
    *port = -1;
  } else {
    GPR_ASSERT(sp != nullptr);
    *port = sp->port;
  }
  socket->listener = sp;
  return error;
}

static void tcp_server_start(grpc_tcp_server* server,
                             const std::vector<grpc_pollset*>* /*pollsets*/,
                             grpc_tcp_server_cb on_accept_cb, void* cb_arg) {
  grpc_tcp_listener* sp;
  GRPC_CUSTOM_IOMGR_ASSERT_SAME_THREAD();
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "SERVER_START %p", server);
  }
  GPR_ASSERT(on_accept_cb);
  GPR_ASSERT(!server->on_accept_cb);
  server->on_accept_cb = on_accept_cb;
  server->on_accept_cb_arg = cb_arg;
  for (sp = server->head; sp; sp = sp->next) {
    grpc_custom_socket* new_socket = static_cast<grpc_custom_socket*>(
        gpr_malloc(sizeof(grpc_custom_socket)));
    new_socket->endpoint = nullptr;
    new_socket->listener = nullptr;
    new_socket->connector = nullptr;
    new_socket->refs = 1;
    grpc_custom_socket_vtable->accept(sp->socket, new_socket,
                                      custom_accept_callback);
  }
}

static unsigned tcp_server_port_fd_count(grpc_tcp_server* /*s*/,
                                         unsigned /*port_index*/) {
  return 0;
}

static int tcp_server_port_fd(grpc_tcp_server* /*s*/, unsigned /*port_index*/,
                              unsigned /*fd_index*/) {
  return -1;
}

static void tcp_server_shutdown_listeners(grpc_tcp_server* s) {
  for (grpc_tcp_listener* sp = s->head; sp; sp = sp->next) {
    if (!sp->closed) {
      sp->closed = true;
      grpc_custom_socket_vtable->close(sp->socket, custom_close_callback);
    }
  }
}

static grpc_core::TcpServerFdHandler* tcp_server_create_fd_handler(
    grpc_tcp_server* /*s*/) {
  return nullptr;
}

grpc_tcp_server_vtable custom_tcp_server_vtable = {
    tcp_server_create,        tcp_server_start,
    tcp_server_add_port,      tcp_server_create_fd_handler,
    tcp_server_port_fd_count, tcp_server_port_fd,
    tcp_server_ref,           tcp_server_shutdown_starting_add,
    tcp_server_unref,         tcp_server_shutdown_listeners};
