#include <iostream>
#include <bits/unique_ptr.h>
#include "proxy_server.h"

proxy_server::proxy_server(io_service &service, ipv4_endpoint endpoint)
        : service(service),
          endpoint(endpoint),
          server(service, endpoint, std::bind(&proxy_server::create_new_left_side, this)),
          resolver(5),
          proxy_cache(10000)
{
    std::cerr << "Proxy server bind on " << server.get_local_endpoint().to_string() << "\n";
}

posix_socket &proxy_server::get_server() {
    return server.get_socket();
}

io_service &proxy_server::get_service() {
    return service;
}

void proxy_server::create_new_left_side() {
    std::unique_ptr<left_side> u_ptr(new left_side(this, [this](left_side* item) {left_sides.erase(item);}));
    left_side *ptr = u_ptr.get();
    left_sides.emplace(ptr, std::move(u_ptr));
}

right_side *proxy_server::create_new_right_side(left_side *caller) {
    std::unique_ptr<right_side> u_ptr(new right_side(this, caller, [this](right_side* item) {right_sides.erase(item);}));
    right_side *ptr = u_ptr.get();
    right_sides.emplace(ptr, std::move(u_ptr));
    return ptr;
}

dns_resolver &proxy_server::get_resolver() {
    return resolver;
}


left_side::left_side(proxy_server *proxy, std::function<void(left_side*)> on_disconnect)
        : proxy(proxy),
          socket(proxy->get_server().accept()),
          partner(nullptr),
          ioEvent(proxy->get_service(), socket.get_fd(), EPOLLIN, [this] (uint32_t events) mutable throw(std::runtime_error)
          {
              //std::cerr << "In left side of " << this << ":" << epoll_event_to_str(events) << "\n";
              if (events & EPOLLIN) {
                  if (read_request()) { return;}
              }
              if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                  this->on_disconnect(this);
              }
              if (events & EPOLLOUT) {
                  send_response();
              }

          }),
          on_disconnect(on_disconnect),
          on_read(true),
          on_write(false)
{
    //std::cerr << "> Left_side created\n";
}

left_side::~left_side() {
    while (connected.size()) {
        (*connected.begin())->on_disconnect(*connected.begin());
    }
    connected.clear();
    //std::cerr << "> Left_side destroyed\n";
}

int left_side::read_request() {
    std::string buffer;
    if (socket.read_input(buffer) == -1) {
        on_disconnect(this);
        return 1;
    }

    if (request.get() == nullptr) {
        request.reset(new http_request(buffer));
    } else {
        request->add_part(buffer);
    }

    if (request->get_state() == http_request::BAD) {
        messages.push(http_wrapper::BAD_REQUEST());
        set_on_write(true);
    } else if (request->get_state() == http_request::FULL_BODY) {
        partner = proxy->create_new_right_side(this);
        connected.insert(partner);
        request.release();
    }
    return 0;
}

void left_side::send_response() {
    while (!messages.empty()) {
        socket.write(messages.front());
        messages.pop();
    }

    if (partner) {
        partner->set_on_read(true);
    }
    set_on_write(false);
}

void left_side::update_state() {
    uint32_t flags = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (on_read) {
        flags |= EPOLLIN;
    }
    if (on_write) {
        flags |= EPOLLOUT;
    }
    ioEvent.modify(flags);
}

void left_side::set_on_read(bool state) {
    on_read = state;
    update_state();
}

void left_side::set_on_write(bool state) {
    on_write = state;
    update_state();
}

right_side::right_side(proxy_server *proxy, left_side *partner, std::function<void(right_side *)> on_disconnect)
        : socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK),
          partner(partner),
          ioEvent(proxy->get_service(), socket.get_fd(), 0, [this] (uint32_t events) mutable throw(std::runtime_error)
          {
       //       std::cerr << "In right of " << this << ":" << epoll_event_to_str(events) << "\n";
//              std::cerr << "My URI is " << request->get_URI() << "\n";
              if (!connected && (events == EPOLLHUP)) {
                  create_connection();
                  return;
              }
              if (events & EPOLLIN) {
                  if (read_response()) { return;}
              }
              if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                  this->on_disconnect(this);
                  return;
              }
              if (events & EPOLLOUT) {
                  send_request();
              }
          }),
          on_disconnect(on_disconnect),
          on_read(false),
          on_write(false),
          proxy(proxy),
          connected(false),
          request(std::move(partner->request)),
          cache_hit(false)
{
    resolver_id = this->proxy->get_resolver().resolve(request->get_host());
    std::cerr << "> Right_side created\n";
}

right_side::~right_side() {
    if (partner != nullptr){
        partner->connected.erase(this);
        if (partner->partner == this) {
            partner->partner = nullptr;
        }
    }
    if (!connected) {
        proxy->get_resolver().cancel(resolver_id);
    }
    try_cache();
    std::cerr << "> Right_side destroyed\n";
}


void right_side::create_connection() {
    sockaddr x;
    socklen_t y;
    bool err_flag;
    if (proxy->get_resolver().result_is_ready(resolver_id, x, y, err_flag)) {
        if (!err_flag) {
            socket.connect(&x, y);
            connected = true;
            set_on_write(true);
        } else {
            on_disconnect(this);
        }
    }
}


void right_side::send_request() {
    if (partner != nullptr) {
        host = request->get_host();
        URI = request->get_URI();
        auto is_valid = request->is_validating();
        cache_hit = proxy->proxy_cache.contains(host + URI);
        if (!is_valid && cache_hit) {
            std::cerr << "Proxy cache hit\n";
            auto cache_entry = proxy->proxy_cache.get(host + URI);
            auto etag = cache_entry.get_header("Etag");
            request->append_header("If-None-Match", etag);
        }

        socket.write(request->get_request_text());
        set_on_read(true);
        set_on_write(false);
    } else {
        on_disconnect(this);
    }
}

int right_side::read_response() {
    if (partner != nullptr) {
        std::string buffer;

        if (socket.read_input(buffer) == -1) {
            on_disconnect(this);
            return 1;
        }
        std::string sub(buffer);
        if (!read_after_cache_hit) {
            if (response.get() == nullptr) {
                response.reset(new http_response(sub));
            } else {
                response->add_part(sub);
            }

            if (response->get_state() >= http_request::FIRST_LINE) {
                if (response->get_code() == "304" && cache_hit) {
                    std::cerr << "Valid cache for " << host << " " << URI << "\n";
                    partner->messages.push(proxy->proxy_cache.get(host + URI).get_text());
                    read_after_cache_hit = true;
                } else {
                    std::cerr << "Not valid cache for " << host << " " << URI << "\n";
                    cache_hit = false;
                    partner->messages.push(sub);
                }
                partner->set_on_write(true);
            }
        } else {
            std::cerr << "Read after get cache\n";
        }
        return 0;
    } else {
        on_disconnect(this);
        return 1;
    }
}

void right_side::update_state() {
    uint32_t flags = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (on_read) {
        flags |= EPOLLIN;
    }
    if (on_write) {
        flags |= EPOLLOUT;
    }
    ioEvent.modify(flags);
}


void right_side::set_on_read(bool state) {
    on_read = state;
    update_state();
}

void right_side::set_on_write(bool state) {
    on_write = state;
    update_state();
}

void right_side::try_cache() {
    if (response && response->is_cacheable() && !cache_hit) {
        proxy->proxy_cache.put(host + URI, http_response(*response));
        std::cerr << "!!!" << host + URI << " cached\n";
    }
}


