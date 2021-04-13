#pragma once

#include <ares.h>

#include <boost/algorithm/string.hpp>
#include <fmt/format.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace ares {

inline void check(int result, const char* what)
{
    if (result != ARES_SUCCESS)
        throw std::runtime_error(what + std::string(": ") + ares_strerror(result));
}

inline void library_init()
{
    check(ares_library_init(ARES_LIB_INIT_ALL), "ares_library_init");
}

inline void library_cleanup() noexcept
{
    ares_library_cleanup();
}

inline int process_work(ares_channel channel)
{
    fd_set readers = {};
    fd_set writers = {};

    timeval tv = {};
    tv.tv_sec = 1;

    while (true)
    {
        FD_ZERO(&readers);
        FD_ZERO(&writers);

        const int nfds = ares_fds(channel, &readers, &writers);
        if (nfds == 0)
            return 0;

        const int count = select(nfds, &readers, &writers, nullptr, &tv);
        if (count < 0)
            return errno;

        ares_process(channel, &readers, &writers);
    }
}

// C++ wrapper for struct hostent
struct HostEnt
{
    std::string name; // Official name of host
    std::vector<std::string> aliases;  // Alias list
    int address_type = -1;  // AF_INET or AF_INET6
    std::vector<std::string> addresses;  // IPv4 or IPv6 address list

    HostEnt() = default;

    explicit HostEnt(struct hostent* hostent)
    {
        if (hostent->h_name)
            name = hostent->h_name;

        if (hostent->h_aliases)
        {
            char** alias_ptr = hostent->h_aliases;
            while (*alias_ptr != nullptr)
            {
                aliases.push_back(*alias_ptr);
                alias_ptr++;
            }
        }

        address_type = hostent->h_addrtype;

        if (hostent->h_addr_list)
        {
            char** address_ptr = hostent->h_addr_list;
            while (*address_ptr != nullptr)
            {
                char buffer[64];
                addresses.push_back(ares_inet_ntop(hostent->h_addrtype, *address_ptr, buffer, sizeof(buffer)));
                address_ptr++;
            }
        }
    }

    std::string to_string() const
    {
        using boost::algorithm::join;
        return fmt::format("{{'{}' aliases=[{}] addresses=[{}]}}", name, join(aliases, ", "), join(addresses, ", "));
    }
};

struct ResolveHostResult
{
    // Whether the callback has been invoked
    bool done = false;

    int status = 0;
    int timeouts = 0;
    HostEnt host;
};

inline void resolve_host_callback(void* arg, int status, int timeouts, struct hostent* hostent)
{
    ResolveHostResult* result = reinterpret_cast<ResolveHostResult*>(arg);
    result->done = true;
    result->status = status;
    result->timeouts = timeouts;
    if (hostent != nullptr)
        result->host = HostEnt(hostent);
}

inline HostEnt resolve_host(const std::string& host_name, int family)
{
    ResolveHostResult result;

    ares_channel channel = nullptr;
    check(ares_init(&channel), "ares_init");
    ares_gethostbyname(channel, host_name.c_str(), family, resolve_host_callback, &result);
    process_work(channel);
    ares_destroy(channel);

    check(result.status, "resolve_host");

    if (!result.done)
        throw std::runtime_error("resolve_host: callback is not invoked");

    if (result.host.addresses.empty())
        throw std::runtime_error("resolve_host: address list is empty");

    return result.host;
}

} // namespace ares
