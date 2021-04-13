#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/process.hpp>

#include <CLI/CLI.hpp>

#include <libcpuid.h>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

#ifdef __linux__
#include <sys/utsname.h>
#endif

#include <bitset>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <queue>

#include "ares.hpp"
#include "go_rng_seed_recovery.hpp"
#include "hardcode.hpp"
#include "serialization.hpp"

namespace http = boost::beast::http;
using boost::asio::ip::tcp;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::details::null_mutex;

static const char USER_AGENT[] = "goldrush-client";

struct ProgramOptions
{
    // Server Options
    std::string address;
    unsigned short port = 8000;

    // Client options
    unsigned algorithm_id = 2;
    unsigned lifetime = LIFETIME + FIRST_API_CALL_TIMEOUT;
    unsigned rps_limit = 0;
    unsigned explore_rps_limit = 800;
    unsigned license_rps_limit = 0;
    unsigned score_limit = 0;

    // Logging Options
    spdlog::level::level_enum log_level = spdlog::level::debug;
    bool no_color = false;
};

template <typename T, typename Mutex>
class Queue
{
public:
    Queue() = default;

    size_t size() const
    {
        const std::unique_lock<Mutex> lock(mutex);
        return queue.size();
    }

    void push(const T& value)
    {
        const std::unique_lock<Mutex> lock(mutex);
        queue.push(value);
    }

    void push_all(const std::vector<T>& values)
    {
        const std::unique_lock<Mutex> lock(mutex);
        for (const auto& value : values)
            queue.push(value);
    }

    std::optional<T> try_pop()
    {
        const std::unique_lock<Mutex> lock(mutex);
        if (queue.empty())
            return std::nullopt;

        auto value = queue.front();
        queue.pop();
        return value;
    }

private:
    std::queue<T> queue;
    mutable std::mutex mutex;
};

template <typename T, typename Compare, typename Mutex>
class PriorityQueue
{
public:
    PriorityQueue() = default;

    size_t size() const
    {
        const std::unique_lock<Mutex> lock(mutex);
        return queue.size();
    }

    void push(const T& value)
    {
        const std::unique_lock<Mutex> lock(mutex);
        queue.push(value);
    }

    void push_all(const std::vector<T>& values)
    {
        const std::unique_lock<Mutex> lock(mutex);
        for (const auto& value : values)
            queue.push(value);
    }

    std::optional<T> try_pop()
    {
        const std::unique_lock<Mutex> lock(mutex);
        if (queue.empty())
            return std::nullopt;

        auto value = queue.top();
        queue.pop();
        return value;
    }

private:
    std::priority_queue<T, std::vector<T>, Compare> queue;
    mutable Mutex mutex;
};

struct CompareReportByAmount
{
    bool operator()(const Report& lhs, const Report& rhs)
    {
        return lhs.amount < rhs.amount;
    }
};

using ReportQueue = PriorityQueue<Report, CompareReportByAmount, std::mutex>;

class TreasureQueue
{
public:
    TreasureQueue() = default;

    size_t size() const
    {
        const std::unique_lock<std::mutex> lock(mutex);
        return treasure_count;
    }

    bool empty() const
    {
        const std::unique_lock<std::mutex> lock(mutex);
        return treasure_count == 0;
    }

    void push(const TreasureInfo& treasure)
    {
        const std::unique_lock<std::mutex> lock(mutex);
        treasures.at(treasure.depth - 1).push_back(treasure);
        treasure_count++;
    }

    std::optional<TreasureInfo> try_pop()
    {
        const std::unique_lock<std::mutex> lock(mutex);
        if (treasure_count == 0)
            return std::nullopt;

        const auto end = treasures.rend();
        for (auto it = treasures.rbegin(); it != end; ++it)
        {
            if (!it->empty())
            {
                auto value = it->back();
                it->pop_back();
                treasure_count--;
                return value;
            }
        }
        return std::nullopt;
    }

    std::optional<TreasureInfo> try_pop(int depth)
    {
        const std::unique_lock<std::mutex> lock(mutex);
        auto& queue = treasures.at(depth - 1);
        if (queue.empty())
            return std::nullopt;

        auto value = queue.back();
        queue.pop_back();
        treasure_count--;
        return value;
    }

    std::bitset<MAX_DEPTH> available_depths() const
    {
        const std::unique_lock<std::mutex> lock(mutex);
        std::bitset<MAX_DEPTH> result;
        for (int i = 0; i < MAX_DEPTH; ++i)
            if (!treasures.at(i).empty())
                result.set(i);
        return result;
    }

    std::array<unsigned, MAX_DEPTH> treasures_per_depth() const
    {
        const std::unique_lock<std::mutex> lock(mutex);
        std::array<unsigned, MAX_DEPTH> result;
        for (int i = 0; i < MAX_DEPTH; ++i)
            result.at(i) = treasures.at(i).size();
        return result;
    }

private:
    std::array<std::vector<TreasureInfo>, MAX_DEPTH> treasures;
    size_t treasure_count = 0;
    mutable std::mutex mutex;
};

template <typename T>
class ScopeIncrement
{
public:
    explicit ScopeIncrement(T& value) : value(value) { ++value; }
    ~ScopeIncrement() { --value; }

    ScopeIncrement(const ScopeIncrement&) = delete;
    ScopeIncrement& operator=(const ScopeIncrement&) = delete;

private:
    T& value;
};

template <typename Mutex>
class AreaSequence
{
public:
    AreaSequence(int area_size_x, int area_size_y, Area outer_area)
        : area{ outer_area.pos_x, outer_area.pos_y, area_size_x, area_size_y }
        , outer_area(outer_area)
    {}

    bool has_next() const
    {
        const std::unique_lock<Mutex> lock(mutex);

        return outer_area.contains(area);
    }

    std::optional<Area> next()
    {
        const std::unique_lock<Mutex> lock(mutex);

        if (!outer_area.contains(area))
            return std::nullopt;

        const Area result = area;

        area.pos_y += area.size_y;
        if (!outer_area.contains(area))
        {
            area.pos_y = outer_area.pos_y;
            area.pos_x += area.size_x;
        }

        return result;
    }

private:
    mutable Mutex mutex;
    Area area;
    const Area outer_area;
};

class RpsLimiter
{
public:
    RpsLimiter() = default;

    void set_limit(size_t rps_limit)
    {
        set_limit(rps_limit, rps_limit);
    }

    void set_limit(size_t rps_limit, size_t rps_burst_limit)
    {
        const std::unique_lock<std::mutex> lock(mutex);

        buffer.clear();
        buffer.set_capacity(rps_burst_limit);

        this->rps_limit = rps_limit;
        this->rps_burst_limit = rps_burst_limit;
    }

    void wait()
    {
        const std::unique_lock<std::mutex> lock(mutex);

        if (rps_limit == 0)
            return;

        buffer.push_back(stopwatch.elapsed().count());
        if (buffer.full())
        {
            const double interval = double(rps_burst_limit) / double(rps_limit);
            while (stopwatch.elapsed().count() - buffer.front() <= interval)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            buffer.back() = stopwatch.elapsed().count();
        }
    }

private:
    const spdlog::stopwatch stopwatch;
    boost::circular_buffer<double> buffer;
    size_t rps_limit;
    size_t rps_burst_limit;
    mutable std::mutex mutex;
};

class LicenseVault
{
public:
    explicit LicenseVault(unsigned capacity)
        : licenses(capacity)
    {
    }

    void add(const License& license)
    {
        const std::unique_lock<std::mutex> lock(mutex);

        if (licenses.full())
            throw std::logic_error("no place for new license");

        licenses.push_back(license);
    }

    std::optional<LicenseId> use_license()
    {
        const std::unique_lock<std::mutex> lock(mutex);

        if (licenses.empty())
            return std::nullopt;

        auto& license = licenses.front();
        const LicenseId license_id = license.id;
        license.dig_used++;
        if (license.expired())
            licenses.pop_front();
        return license_id;
    }

    size_t license_count() const
    {
        const std::unique_lock<std::mutex> lock(mutex);
        
        return licenses.size();
    }

    size_t use_count() const
    {
        const std::unique_lock<std::mutex> lock(mutex);

        size_t count = 0;
        for (const auto& license : licenses)
            count += static_cast<size_t>(license.dig_allowed - license.dig_used);
        return count;
    }

    size_t capacity() const
    {
        return licenses.capacity();
    }

private:
    boost::circular_buffer<License> licenses;
    mutable std::mutex mutex;
};

class CoinVault
{
public:
    CoinVault() = default;

    void add(const Wallet& wallet)
    {
        const std::unique_lock<std::mutex> lock(mutex);

        for (const auto& coin : wallet)
            coins.push_back(coin);
    }

    Wallet try_take(Amount amount)
    {
        const std::unique_lock<std::mutex> lock(mutex);

        Wallet wallet;
        if (amount <= coins.size())
        {
            wallet.reserve(amount);
            for (Amount i = 0; i < amount; ++i)
            {
                wallet.push_back(coins.back());
                coins.pop_back();
            }
        }
        return wallet;
    }

    size_t count() const
    {
        const std::unique_lock<std::mutex> lock(mutex);

        return coins.size();
    }

private:
    std::vector<Coin> coins;
    mutable std::mutex mutex;
};

class TreasureMap
{
public:
    TreasureMap() : data(MAP_SIZE * MAP_SIZE) {}

    void put_treasure(int x, int y, int depth)
    {
        data.at(offset(x, y)).set(depth - 1);
    }

    bool has_treasure(int x, int y, int depth) const
    {
        return data.at(offset(x, y)).test(depth - 1);
    }

    std::bitset<MAX_DEPTH> treasures(int x, int y) const
    {
        return data.at(offset(x, y));
    }

private:
    std::vector<std::bitset<MAX_DEPTH>> data;

    size_t offset(int x, int y) const
    {
        return static_cast<size_t>(y) * MAP_SIZE + static_cast<size_t>(x);
    }
};

class DigMap
{
public:
    DigMap() : data(MAP_SIZE) {}

    bool set(int x, int y)
    {
        const std::unique_lock<std::mutex> lock(mutex);
        if (data.at(y).test(x))
            return false;
        data.at(y).set(x);
        return true;
    }

    bool test(int x, int y) const
    {
        const std::unique_lock<std::mutex> lock(mutex);
        return data.at(y).test(x);
    }

private:
    std::vector<std::bitset<MAP_SIZE>> data;
    mutable std::mutex mutex;
};

void fill_treasure_map(TreasureMap& map, go_rng_seed_recovery& rand, unsigned treasure_count)
{
    for (unsigned i = 0; i < treasure_count; ++i)
    {
        const int x = rand.int31n<MAP_SIZE>();
        const int y = rand.int31n<MAP_SIZE>();
        const int depth = rand.int31n<MAX_DEPTH>() + 1;

        map.put_treasure(x, y, depth);
    }
}

struct TreasurePlace
{
    int pos_x = 0;
    int pos_y = 0;
    std::bitset<MAX_DEPTH> treasures;

    int depth() const
    {
        for (int i = 0; i < MAX_DEPTH; ++i)
            if (treasures.test(i))
                return i + 1;
        return 0;
    }
};

std::vector<TreasurePlace> find_treasure_places(const TreasureMap& map)
{
    std::vector<TreasurePlace> result;
    result.reserve(TREASURE_COUNT);

    for (int y = 0; y < MAP_SIZE; ++y)
    {
        for (int x = 0; x < MAP_SIZE; ++x)
        {
            const auto treasures = map.treasures(x, y);
            if (treasures.any())
                result.push_back(TreasurePlace{ x, y, treasures });
        }
    }
    return result;
}

std::vector<TreasurePlace> order_treasure_places(const std::vector<TreasurePlace>& treasure_places, int game_type)
{
    std::vector<TreasurePlace> result;
    result.reserve(treasure_places.size());

    std::array<std::vector<TreasurePlace>, MAX_DEPTH> treasures_per_level;
    for (auto& vec : treasures_per_level)
        vec.reserve(treasure_places.size() / MAX_DEPTH);

    const int drop_level = DROP_LEVELS[game_type - 1];
    const auto main_levels = MAIN_LEVELS[game_type - 1];

    for (const auto& place : treasure_places)
    {
        const auto treasures = (place.treasures >> drop_level) << drop_level;
        if (treasures.count() > 1)
        {
            result.push_back(place);
        }
        else if (treasures.count() == 1)
        {
            for (int i = 0; i < MAX_DEPTH; ++i)
            {
                if (treasures.test(i))
                {
                    treasures_per_level.at(i).push_back(place);
                    break;
                }
            }
        }
    }

    for (;;)
    {
        const auto count = result.size();
        for (int i = 0; i < MAX_DEPTH; ++i)
        {
            if (main_levels.test(i))
            {
                auto& vec = treasures_per_level.at(i);
                if (!vec.empty())
                {
                    result.push_back(vec.back());
                    vec.pop_back();
                }
            }
        }
        if (count == result.size())
            break;
    }

    for (int i = 0; i < MAX_DEPTH; ++i)
    {
        if (!main_levels.test(i))
        {
            auto& vec = treasures_per_level.at(i);
            result.insert(result.end(), vec.begin(), vec.end());
        }
    }

    return result;
}

using TreasurePlaceQueue = Queue<TreasurePlace, std::mutex>;

std::string get_exception_info(std::exception_ptr exception)
{
    try
    {
        std::rethrow_exception(exception);
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    catch (...)
    {
        return "<unknown error>";
    }
}

std::string current_exception_info()
{
    return get_exception_info(std::current_exception());
}

template <typename Key, typename Value>
std::string join_values(const std::map<Key, Value>& map, std::string_view separator)
{
    std::string result;
    for (const auto& [key, value] : map)
    {
        if (!result.empty())
            result += separator;
        result += fmt::format("{}: {}", key, value);
    }
    return result;
}

template <typename Value>
std::string join_values(const std::set<Value>& set, std::string_view separator)
{
    std::string result;
    for (const auto& value : set)
    {
        if (!result.empty())
            result += separator;
        result += fmt::format("{}", value);
    }
    return result;
}

template <typename Callable>
void run_parallel(int thread_count, Callable&& callable)
{
    auto run = [&](int thread_num)
    {
        try
        {
            callable(thread_num);
        }
        catch (...)
        {
            spdlog::error("Exception: {}", current_exception_info());
        }
    };

    if (thread_count == 1)
    {
        run(0);
    }
    else
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < thread_count; ++i)
            threads.emplace_back(run, i);

        for (auto& thread : threads)
            thread.join();
    }
}

class GameTypeDetector
{
public:
    GameTypeDetector()
    {
        data.fill(0x3ff);
    }

    std::optional<unsigned> game_type() const
    {
        const std::unique_lock<std::mutex> lock(mutex);

        std::optional<unsigned> result;
        for (unsigned i = 0; i < data.size(); ++i)
        {
            if ((data.at(i) & mask).count() == mask.count())
            {
                if (result)
                    return std::nullopt;
                
                result = i + 1;
            }
        }
        return result;
    }

    void update(int depth, Amount cost)
    {
        const std::unique_lock<std::mutex> lock(mutex);

        for (unsigned i = 0; i < data.size(); ++i)
        {
            const auto cost_range = TREASURE_COST_RANGES[i][depth - 1];
            if (!cost_range.contains(cost))
                data.at(i).reset(depth - 1);
        }
        mask.set(depth - 1);
    }

private:
    std::array<std::bitset<MAX_DEPTH>, GAME_TYPE_COUNT> data;
    std::bitset<MAX_DEPTH> mask = 0;
    mutable std::mutex mutex;
};

class Statistics
{
public:
    void treasures_found(Amount treasure_count)
    {
        total_treasures_found += treasure_count;
        {
            const std::unique_lock<std::mutex> lock(mutex);
            treasures_amount[treasure_count]++;
        }
    }

    void treasures_digged(int depth, Amount treasure_count)
    {
        total_treasures_digged += treasure_count;
    }

    void treasure_sold(int depth, Amount cost)
    {
        {
            const std::unique_lock<std::mutex> lock(mutex);
            treasures_cost_per_depth[depth][cost]++;
        }
        total_treasures_sold++;
    }

    void treasure_dropped()
    {
        total_treasures_dropped++;
    }

    void print_report()
    {
        info("Treasures found: {}", total_treasures_found.load());
        info("Treasures digged: {}", total_treasures_digged.load());
        info("Treasures sold: {}", total_treasures_sold.load());
        info("Treasures dropped: {}", total_treasures_dropped.load());

        const std::unique_lock<std::mutex> lock(mutex);

        for (const auto& [depth, cost] : treasures_cost_per_depth)
            info("Treasures at {}: {}", depth, join_values(cost, ", "));

        for (const auto [amount, count] : treasures_amount)
            info("Treasures {} * {} = {} ({:.2f}%)", amount, count,
                amount * count,
                amount * count * 100.0 / total_treasures_found);
    }

private:
    std::mutex mutex;
    std::map<int, std::map<Amount, unsigned>> treasures_cost_per_depth;
    std::map<Amount, unsigned> treasures_amount;

    std::atomic_uint64_t total_treasures_found = 0;
    std::atomic_uint64_t total_treasures_digged = 0;
    std::atomic_uint64_t total_treasures_sold = 0;
    std::atomic_uint64_t total_treasures_dropped = 0;
};

class Client
{
public:
    Client(boost::asio::io_context& ioc, const tcp::endpoint& endpoint, const std::string& host)
        : ioc(ioc)
        , endpoint(endpoint)
        , host(host)
    {}

    boost::asio::io_context& ioc;
    const tcp::endpoint endpoint;
    const std::string host;
    const spdlog::stopwatch stopwatch;
    std::set<http::status> auto_retry;
    std::atomic_bool throw_exceptions = true;
    RpsLimiter rps_limiter;
    RpsLimiter explore_rps_limiter;
    RpsLimiter license_rps_limiter;

    bool need_stop() const
    {
        return ioc.stopped();
    }

    void stop()
    {
        ioc.stop();
    }

    bool wait_server_start(std::chrono::milliseconds timeout = {})
    {
        info("Wait server response...");
        spdlog::stopwatch stopwatch;
        while (!ioc.stopped() && (timeout.count() == 0 || stopwatch.elapsed() < timeout))
        {
            try
            {
                http::request<http::string_body> request{ http::verb::get, "/health-check", 11 };
                request.set(http::field::host, host);
                request.set(http::field::user_agent, USER_AGENT);

                http::response<http::string_body> response;
                {
                    const auto conn = create_connection();
                    http::write(*conn, request);

                    boost::beast::flat_buffer read_buffer;
                    http::read(*conn, read_buffer, response);

                    conn->socket().close();
                }
                
                if (response.result() == http::status::ok)
                {
                    info("Response: {}", boost::lexical_cast<std::string>(response));
                    return true;
                }
                spdlog::error("Response: {}", boost::lexical_cast<std::string>(response));
            }
            catch (const boost::system::system_error& e)
            {
                spdlog::error(e.what());
            }
        }
        return false;
    }

    std::unique_ptr<boost::beast::tcp_stream> create_connection()
    {
        unsigned retry_count = 0;
        for (;;)
        {
            try
            {
                auto conn = std::make_unique<boost::beast::tcp_stream>(ioc);
                conn->connect(endpoint);
                conn->socket().set_option(boost::asio::socket_base::linger(true, 0));
                conn->socket().set_option(tcp::no_delay(true));
                return conn;
            }
            catch (...)
            {
                if (++retry_count > 3 || need_stop())
                    throw;
            }
        }
    }

    template <typename Value>
    http::request<http::string_body> create_request(http::verb verb, std::string_view path, const Value& value)
    {
        http::request<http::string_body> request{ verb, path, 11 };
        request.set(http::field::host, host);
        request.set(http::field::user_agent, USER_AGENT);
        if (verb == http::verb::post)
            request.set(http::field::content_type, "application/json");

        StdStringWrapper output_stream(request.body());
        rapidjson::Writer<StdStringWrapper> writer(output_stream);

        write_value(writer, value);
        request.prepare_payload();

        return request;
    }

    template <typename Value>
    struct Result
    {
        std::optional<Value> value;
        std::optional<http::response<http::string_body>> response;
        std::exception_ptr exception;
        double request_time = 0.0;
        double write_read_time = 0.0;
    };

    Result<Balance> get_balance()
    {
        return get<Balance>("/balance");
    }

    Result<LicenseList> list_licenses()
    {
        return get<LicenseList>("/licenses");
    }

    Result<License> issue_license(const Wallet& wallet)
    {
        return post<Wallet, License>("/licenses", wallet);
    }

    Result<Report> explore_area(int pos_x, int pos_y, int size_x, int size_y)
    {
        return explore_area(Area{ pos_x, pos_y, size_x, size_y });
    }

    Result<Report> explore_area(const Area& area)
    {
        return post<Area, Report>("/explore", area);
    }

    Result<TreasureList> dig(const LicenseId license_id, int pos_x, int pos_y, int depth)
    {
        return post<Dig, TreasureList>("/dig", Dig{ license_id, pos_x, pos_y, depth });
    }

    Result<Wallet> cash(const Treasure& treasure)
    {
        return post<Treasure, Wallet>("/cash", treasure);
    }

    template <typename ResponseValue>
    Result<ResponseValue> get(std::string_view path)
    {
        return api_call<std::nullptr_t, ResponseValue>(http::verb::get, path, nullptr);
    }

    template <typename RequestValue, typename ResponseValue>
    Result<ResponseValue> post(std::string_view path, const RequestValue& request_value)
    {
        return api_call<RequestValue, ResponseValue>(http::verb::post, path, request_value);
    }

    template <typename ResponseValue>
    bool need_retry(const Result<ResponseValue>& result) const
    {
        if (result.response.has_value())
        {
            return auto_retry.count(result.response->result()) != 0;
        }
        return false;
    }

    template <typename RequestValue, typename ResponseValue>
    Result<ResponseValue> api_call(http::verb verb, std::string_view path, const RequestValue& request_value)
    {
        Result<ResponseValue> result;
        do
        {
            result = do_api_call<RequestValue, ResponseValue>(verb, path, request_value);
        } while (!need_stop() && need_retry(result));

        return result;
    }

    template <typename RequestValue, typename ResponseValue>
    Result<ResponseValue> do_api_call(http::verb verb, std::string_view path, const RequestValue& request_value)
    {
        rps_limiter.wait();

        if (verb == http::verb::post && path == "/explore")
            explore_rps_limiter.wait();

        if (verb == http::verb::post && path == "/licenses")
            license_rps_limiter.wait();

        Result<ResponseValue> result;
        spdlog::stopwatch request_stopwatch;
        try
        {
            http::request<http::string_body> request{ verb, path, 11 };
            request.set(http::field::host, host);
            request.set(http::field::user_agent, USER_AGENT);
            if (verb == http::verb::post)
                request.set(http::field::content_type, "application/json");

            StdStringWrapper output_stream(request.body());
            rapidjson::Writer<StdStringWrapper> writer(output_stream);

            write_value(writer, request_value);
            request.prepare_payload();
            
            boost::beast::flat_buffer read_buffer;
            http::response<http::string_body> response;

            const auto conn = create_connection();
            {
                spdlog::stopwatch stopwatch;

                http::write(*conn, request);
                http::read(*conn, read_buffer, response);

                result.write_read_time = stopwatch.elapsed().count();
                result.response = std::move(response);
            }
            conn->socket().close();

            if (result.response && result.response->result() == http::status::ok)
            {
                rapidjson::Document document;
                parse_json(result.response->body(), document);

                result.value = read_value<ResponseValue>(document);
            }
        }
        catch (...)
        {
            result.exception = std::current_exception();
        }

        result.request_time = request_stopwatch.elapsed().count();
        update_metrics(verb, path, request_value, result);

        if (result.exception && throw_exceptions)
            std::rethrow_exception(result.exception);

        return result;
    }

    struct Timings
    {
        std::uint64_t count = 0;
        double total_time = 0.;
        double min_time = 0.;
        double max_time = 0.;

        double avg_time() const
        {
            return count > 0 ? total_time / count : 0.;
        }

        void update(double time)
        {
            if (count == 0)
            {
                min_time = time;
                max_time = time;
            }
            else
            {
                min_time = std::min(min_time, time);
                max_time = std::max(max_time, time);
            }
            total_time += time;
            count++;
        }
    };

    struct ErrorMetrics
    {
        std::uint64_t total_count;
        Timings timings;
        std::map<std::string, std::uint64_t> messages;
    };

    struct RequestMetrics
    {
        std::string request_id;
        std::uint64_t total_count = 0;
        std::uint64_t success_count = 0;
        std::uint64_t exception_count = 0;
        std::uint64_t error_count = 0;
        double begin_time = 0.;
        double end_time = 0.;
        Timings total_timings;
        Timings success_timings;
        Timings exception_timings;
        Timings error_timinigs;
        std::map<std::string, std::uint64_t> exceptions;
        std::map<http::status, ErrorMetrics> errors;

        double elapsed() const
        {
            return end_time - begin_time;
        }
    };

    struct ExploreMetrics
    {};

    struct DigMetrics
    {
        std::map<int, Amount> levels;
    };

    struct CashMetrics
    {
        std::map<int, unsigned> treasure_cost;
        std::map<int, Timings> treasure_cost_timings;
    };

    struct Metrics
    {
        std::map<std::string, RequestMetrics> requests;
        DigMetrics dig;
        CashMetrics cash;
        std::set<std::string> headers;
    };

    Metrics get_metrics() const
    {
        std::unique_lock<std::mutex> lock(metrics_mutex);
        return metrics;
    }

private:
    Metrics metrics;
    mutable std::mutex metrics_mutex;

    template <typename RequestValue, typename ResponseValue>
    void update_metrics(http::verb verb, std::string_view path, const RequestValue& request_value, const Result<ResponseValue>& result)
    {
        const auto request_id = fmt::format("{} {}", http::to_string(verb), path);

        std::unique_lock<std::mutex> lock(metrics_mutex);

        const double current_time = stopwatch.elapsed().count();

        auto& m = metrics.requests[request_id];
        if (m.request_id.empty())
        {
            m.request_id = request_id;
            m.begin_time = current_time;
        }
        m.end_time = current_time;

        m.total_timings.update(result.request_time);
        m.total_count++;

        if (result.value)
        {
            m.success_timings.update(result.request_time);
            m.success_count++;
        }

        if (result.exception)
        {
            m.exception_timings.update(result.request_time);
            m.exception_count++;
            m.exceptions[get_exception_info(result.exception)]++;
        }

        if (result.response && result.response->result() != http::status::ok)
        {
            m.error_timinigs.update(result.request_time);
            m.error_count++;

            auto& error_metrics = m.errors[result.response->result()];
            error_metrics.timings.update(result.request_time);
            error_metrics.total_count++;
            error_metrics.messages[result.response->body()]++;
        }

        if (result.response)
        {
            for (const auto& field : result.response.value())
                if (field.name() != http::field::date && field.name() != http::field::content_length)
                    metrics.headers.insert(fmt::format("{}: {}", field.name_string(), field.value()));
        }

        update_specific_metrics(request_value, result);
    }

    template <typename RequestValue, typename ResponseValue>
    void update_specific_metrics(const RequestValue& request_value, const Result<ResponseValue>& result)
    {
    }

    void update_specific_metrics(const Dig& request_value, const Result<TreasureList>& result)
    {
        if (result.value)
        {
            metrics.dig.levels[request_value.depth] += result.value->size();
        }
    }

    void update_specific_metrics(const Treasure& request_value, const Result<Wallet>& result)
    {
        if (result.value)
        {
            const auto treasure_cost = result.value->size();
            metrics.cash.treasure_cost[treasure_cost]++;
            metrics.cash.treasure_cost_timings[treasure_cost].update(result.request_time);
        }
    }
};

template <typename Callback>
void find_treasures(Client& client, int pos_x, int pos_y, int size_x, Amount amount, Callback&& callback)
{
    if (amount == 0)
        return;

    if (size_x == 1)
    {
        if (amount > 0)
            callback(pos_x, pos_y, amount);
        return;
    }

    const int left_size_x = size_x / 2;
    const int right_size_x = size_x - left_size_x;

    const auto result = client.explore_area(pos_x, pos_y, left_size_x, 1);
    if (!result.value)
        return;

    const Amount left_amount = result.value->amount;
    const Amount right_amount = amount - left_amount;

    find_treasures(client, pos_x, pos_y, left_size_x, left_amount, callback);
    find_treasures(client, pos_x + left_size_x, pos_y, right_size_x, right_amount, callback);
}

template <typename Callback, typename Mutex>
void find_treasures(Client& client, AreaSequence<Mutex> area_seq, Amount available, Callback&& callback)
{
    Amount found = 0;
    while (auto area = area_seq.next())
    {
        if (found >= available)
            break;

        Amount amount = 0;
        if (area_seq.has_next())
        {
            const auto result = client.explore_area(area.value());
            if (!result.value)
                return;
            amount = result.value->amount;
        }
        else
        {
            amount = available - found;
        }
        if (area->size_x == 30)
            find_treasures(client, AreaSequence<spdlog::details::null_mutex>(3, 1, area.value()), amount, callback);
        else
            find_treasures(client, area->pos_x, area->pos_y, area->size_x, amount, callback);
        found += amount;
    }
}

template <typename Callback>
void search_treasures(Client& client, Callback&& callback)
{
    AreaSequence<std::mutex> area_seq(510, 1, MAP_AREA);
    ReportQueue explore_report_queue;

    std::atomic_bool stop_search = false;
    run_parallel(2, [&](int /*thread_num*/)
    {
        while (!client.need_stop() && !stop_search)
        {
            for (int i = 0; i < 2;)
            {
                const auto next_area = area_seq.next();
                if (!next_area)
                    break;

                const auto result = client.explore_area(next_area.value());
                if (!result.value)
                    continue;

                const auto& report = result.value.value();
                if (report.amount > 0)
                {
                    explore_report_queue.push(report);
                    i++;
                }
            }

            const auto report = explore_report_queue.try_pop();
            if (!report)
                break;

            find_treasures(client, AreaSequence<spdlog::details::null_mutex>(30, 1, report->area), report->amount,
                [&](int pos_x, int pos_y, Amount amount)
            {
                if (!stop_search)
                    stop_search = !callback(Report{ Area{pos_x, pos_y}, amount });
            });
        }
    });
}

using RngShortSignature = uint32_t;

struct RngSeedBlock
{
    unsigned start_seed = 0;
    unsigned length = 0;
    RngShortSignature* short_signatures = nullptr;
};

std::vector<RngSeedBlock> generate_rng_seed_blocks(const unsigned block_length,
    char* memory, size_t memory_size)
{
    std::vector<RngSeedBlock> blocks;
    blocks.reserve(std::numeric_limits<int>::max() / block_length + 3);

    size_t offset = 0;

    std::mutex mutex;
    run_parallel(3, [&](int thread_num)
    {
        const auto superblock = go_rng_seed_superblocks[thread_num];
        go_rng_seed_recovery rand(superblock.start_seed);
        unsigned block_num = 0;
        while (superblock.length > block_num * block_length)
        {
            RngSeedBlock block;
            block.start_seed = rand.current_seed();
            block.length = std::min(superblock.length - block_num * block_length, block_length);
            if (memory_size > 0)
            {
                const std::unique_lock<std::mutex> lock(mutex);
                if (offset + block.length * sizeof(RngShortSignature) < memory_size)
                {
                    block.short_signatures = (RngShortSignature*)(memory + offset);
                    offset += block.length * sizeof(RngShortSignature);
                }
            }

            for (unsigned i = 0; i < block.length; ++i)
            {
                if (block.short_signatures != nullptr)
                {
                    RngShortSignature& signature = block.short_signatures[i];
                    signature = 0;
                    for (unsigned bit = 0; bit < sizeof(RngShortSignature); ++bit)
                    {
                        const int a = rand.int31n<100>();
                        const int b = (a >= 60) ? rand.int31n<90'000'000>() : rand.int31n<100>();
                        if (a < 60)
                            signature |= 1ull << bit;
                    }
                }
                rand.next_state();
            }

            {
                const std::unique_lock<std::mutex> lock(mutex);
                blocks.push_back(block);
            }
            ++block_num;
        }
    });
    blocks.push_back({ 2147483647, 1 });
    return blocks;
}

using RngSignature = std::pair<std::bitset<64>, std::bitset<64>>;

RngSignature get_rng_signature(Client& client, LicenseVault& license_vault,
    std::atomic_uint& active_free_license_requests, std::atomic_bool& limited_mode)
{
    std::mutex mutex;
    std::exception_ptr last_exception_ptr;

    RngSignature signature = {};

    auto read_answer = [&](std::unique_ptr<boost::beast::tcp_stream> conn, unsigned i)
    {
        try
        {
            const ScopeIncrement<std::atomic_uint> inc(active_free_license_requests);

            boost::beast::flat_buffer read_buffer;
            http::response<http::string_body> response;
            bool has_response = false;

            spdlog::stopwatch stopwatch;
            while (stopwatch.elapsed() < std::chrono::milliseconds(300))
            {
                if (conn->socket().available() > 0)
                {
                    http::read(*conn, read_buffer, response);
                    has_response = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            conn->socket().close();

            const std::unique_lock<std::mutex> lock(mutex);
            if (has_response)
            {
                rapidjson::Document document;
                switch (response.result_int())
                {
                case 200:
                    parse_json(response.body(), document);
                    license_vault.add(read_value<License>(document));
                    break;

                case 409:
                    break;

                case 502:
                    signature.first.set(i);
                    break;

                case 504:
                    signature.first.set(i);
                    signature.second.set(i);
                    break;

                default:
                    throw std::runtime_error("invalid HTTP response " + std::to_string(response.result_int()));
                }
            }
            else
            {
                signature.first.set(i);
                signature.second.set(i);
            }
        }
        catch (...)
        {
            spdlog::critical("Exception: {}", current_exception_info());

            const std::unique_lock<std::mutex> lock(mutex);
            last_exception_ptr = std::current_exception();
        }
    };

    std::vector<std::thread> threads;
    try
    {
        for (unsigned i = 0; i < 60; ++i)
        {
            auto conn = client.create_connection();
            const auto request = client.create_request(http::verb::post, "/licenses", Wallet{});
            http::write(*conn, request);

            threads.emplace_back(read_answer, std::move(conn), i);

            spdlog::stopwatch stopwatch;
            while (stopwatch.elapsed() < std::chrono::milliseconds(25))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    catch (...)
    {
        spdlog::critical("Exception: {}", current_exception_info());

        const std::unique_lock<std::mutex> lock(mutex);
        last_exception_ptr = std::current_exception();
    }

    limited_mode = false;

    for (auto& thread : threads)
        thread.join();

    if (last_exception_ptr)
        std::rethrow_exception(last_exception_ptr);

    return signature;
}

int find_raw_seed(const RngSignature signature, const int game_type,
    const std::vector<RngSeedBlock>& rng_seeds)
{
    constexpr uint64_t desired_matches = 1ull << 60;

    std::atomic_uint answer = 0;
    std::atomic_uint64_t total_cycles = 0;

    auto search = [&](const unsigned start_seed, const unsigned total_seeds, const RngShortSignature* short_signatures)
    {
        const RngShortSignature s0 = signature.first.to_ullong() & ((1ull << sizeof(RngShortSignature)) - 1);
        if (short_signatures != nullptr && std::numeric_limits<RngShortSignature>::max() > total_seeds)
        {
            const auto end_ptr = short_signatures + total_seeds;
            if (std::find(short_signatures, end_ptr, s0) == end_ptr)
                return;
        }
        
        go_rng_seed_recovery rand(start_seed);

        unsigned i = total_seeds;
        const uint64_t s1 = signature.first.to_ullong();
        const uint64_t s2 = signature.second.to_ullong();
        while (i != 0 && answer == 0)
        {
            --i;

            if ((short_signatures != nullptr && *short_signatures++ != s0) || game_type != rand.int31n<5>())
            {
                rand.next_state();
                continue;
            }

            rand.reset_state();

            uint64_t m = 1;
            while (m != desired_matches)
            {
                const int a = rand.int31n<100>();
                if (a < 60)
                {
                    if ((s1 & m) == 0)
                        break;

                    const int b = rand.int31n<100>();
                    if (b < 10)
                    {
                        if ((s2 & m) == 0)
                            break;
                    }
                    else
                    {
                        if ((s2 & m) != 0)
                            break;
                    }
                }
                else
                {
                    if ((s1 & m) != 0)
                        break;

                    rand.int31n<90'000'000>();
                }

                m <<= 1;
            }

            if (m == desired_matches)
                answer = rand.current_seed();
            else
                rand.next_state();
        }
        total_cycles += total_seeds - i;
    };

    std::atomic_uint block_num = 0;
    run_parallel(CPU_COUNT - 1, [&](int /*thread_num*/)
    {
        while (answer == 0)
        {
            const auto n = block_num++;
            if (n >= rng_seeds.size())
                break;

            const auto block = rng_seeds.at(n);
            search(block.start_seed, block.length, block.short_signatures);
        }
    });

    info("Cycles: {}", total_cycles.load() / 2147483647.0);
    return answer;
}

bool check_raw_seed(Client& client, int raw_seed)
{
    go_rng_seed_recovery rand(raw_seed);

    unsigned matches = 0;
    unsigned fails = 0;
    while (matches < 5)
    {
        const int x = rand.int31n<MAP_SIZE>();
        const int y = rand.int31n<MAP_SIZE>();
        const int depth = rand.int31n<MAX_DEPTH>() + 1;

        if (x < MAP_SIZE / 2 || y < MAP_SIZE / 2)
            continue;

        const auto result = client.explore_area(x, y, 1, 1);
        if (result.value)
        {
            if (result.value->amount == 0)
            {
                spdlog::error("Treasure not found: x={} y={}", x, y);
                return false;
            }
            ++matches;
        }
        else
        {
            if (++fails >= 50)
            {
                spdlog::error("Too many fails in check_raw_seed()");
                return false;
            }
        }
    }
    return true;
}

unsigned find_treasure_count(Client& client, DigMap& dig_map, int raw_seed)
{
    struct Data
    {
        int16_t pos_x = 0;
        int16_t pos_y = 0;
        uint8_t amount = 0;
        uint8_t has_collision = 0;
    };
    std::vector<Data> data(MAX_TREASURE_COUNT);

    go_rng_seed_recovery rand(raw_seed);

    TreasureMap treasure_map;

    for (auto& item : data)
    {
        const int x = rand.int31n<MAP_SIZE>();
        const int y = rand.int31n<MAP_SIZE>();
        const int depth = rand.int31n<MAX_DEPTH>() + 1;

        if (treasure_map.has_treasure(x, y, depth))
            item.has_collision = 1;
        else
            treasure_map.put_treasure(x, y, depth);

        item.pos_x = x;
        item.pos_y = y;
        item.amount = treasure_map.treasures(x, y).count();
    }

    auto predict_treasure_count = [](unsigned x)
    {
        if (x % 100 == 0)
            return x;
        return (x / 100 + 1) * 100;
    };

    unsigned fails = 0;

    unsigned l = MIN_TREASURE_COUNT - 1;
    unsigned r = MAX_TREASURE_COUNT;

    while (r - l > 1)
    {
        unsigned m = l + (r - l) / 2;
        while (m > l && (data.at(m).has_collision || dig_map.test(data[m].pos_x, data[m].pos_y)))
            --m;

        if (m == l)
            return predict_treasure_count(l + 1);

        const auto item = data.at(m);
        
        const auto result = client.explore_area(item.pos_x, item.pos_y, 1, 1);
        if (!result.value || dig_map.test(item.pos_x, item.pos_y))
        {
            if (++fails >= 50)
                throw std::runtime_error("failed to get treasure count");

            continue;
        }

        if (item.amount <= result.value->amount)
            l = m;
        else
            r = m;
    }
    return l + 1;
}

std::optional<TreasureInfo> get_optimal_treasure_01(TreasureQueue& treasure_queue,
    int game_type, go_rng_seed_recovery& rand)
{
    const auto depths = treasure_queue.available_depths();
    if (depths.count() == 0)
        return std::nullopt;

    int optimal_depth = 0;
    double max_profit = 0;

    for (int i = 0; i < MAX_DEPTH; ++i)
    {
        if (depths.test(i))
        {
            const Amount delta = 1 + TREASURE_COST_RANGES[game_type - 1][i].delta();
            const double profit = rand.peek_int31n_fast(delta) / double(delta);
            if (profit > max_profit || optimal_depth == 0)
            {
                optimal_depth = i + 1;
                max_profit = profit;
            }
        }
    }
    return treasure_queue.try_pop(optimal_depth);
}

std::optional<TreasureInfo> get_optimal_treasure(TreasureQueue& treasure_queue,
    int game_type, go_rng_seed_recovery& rand)
{
    if (treasure_queue.empty())
        return std::nullopt;

    auto treasures = treasure_queue.treasures_per_depth();
    const auto next = rand.peek_n_uint64<5>();

    constexpr uint64_t RNG_MASK = (UINT64_C(1) << 63) - 1;

    auto get_profit = [&](unsigned pos, int level)
    {
        const auto delta = 1 + TREASURE_COST_RANGES[game_type - 1][level].delta();
        const auto v = static_cast<uint32_t>((next.at(pos) & RNG_MASK) >> 32);
        const auto max = (1u << 31) - 1 - (1u << 31) % delta;
        if (v > max)
            return -10.0;

        return (v % delta) / double(delta);
    };

    int optimal_depth = 0;
    double max_profit = 0;

    for (int i = 0; i < MAX_DEPTH; ++i)
    {
        if (treasures[i] == 0)
            continue;

        treasures[i]--;
        for (int j = 0; j < MAX_DEPTH; ++j)
        {
            if (treasures[j] == 0)
                continue;

            /*const double profit = get_profit(0, i) + get_profit(1, j);
            if (profit > max_profit || optimal_depth == 0)
            {
                optimal_depth = i + 1;
                max_profit = profit;
            }*/
            
            treasures[j]--;
            for (int k = 0; k < MAX_DEPTH; ++k)
            {
                if (treasures[k] == 0)
                    continue;

                const double profit = get_profit(0, i) + get_profit(1, j) + get_profit(2, k);
                if (profit > max_profit || optimal_depth == 0)
                {
                    optimal_depth = i + 1;
                    max_profit = profit;
                }
            }
            treasures[j]++;
            
        }
        treasures[i]++;
    }
    return treasure_queue.try_pop(optimal_depth);
}

void algorithm_01_basic(Client& client, const ProgramOptions& options)
{
    if (!client.wait_server_start())
        return;

    client.auto_retry.insert(http::status::too_many_requests);
    client.auto_retry.insert(http::status::service_unavailable);

    spdlog::stopwatch stopwatch;
    auto is_first_stage = [&]() { return stopwatch.elapsed() < std::chrono::seconds(1); };
    auto is_final_stage = [&]() { return stopwatch.elapsed() > std::chrono::seconds(LIFETIME - 2); };

    auto sleep = [&]() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };

    GameTypeDetector game_type_detector;

    CoinVault coin_vault;
    LicenseVault license_vault(MAX_LICENSE_COUNT);

    ReportQueue report_queue;
    TreasureQueue treasure_queue;

    Statistics statistics;
    std::atomic_uint64_t report_queue_is_empty = 0;
    std::atomic_uint64_t license_vault_is_empty = 0;
    std::atomic_uint64_t license_vault_and_treasure_queue_is_empty = 0;
    std::atomic_uint64_t full_stop = 0;

    std::atomic_uint active_free_license_requests = 0;
    std::atomic_uint active_paid_license_requests = 0;

    std::atomic_uint game_type = 0;
    std::atomic_int drop_depth = 2;

    constexpr size_t REPORT_MIN_QUEUE_SIZE = 50;
    constexpr size_t TREASURE_MIN_QUEUE_SIZE = 20;

    constexpr int REQUESTS_PER_FREE_LICENSE = 5;

    constexpr bool USE_PAID_LICENSES = false;

    constexpr int EXPLORE_THREADS = 1;
    constexpr int DIG_THREADS = 2;
    constexpr int CASH_THREADS = 3;
    constexpr int FREE_LICENSE_THREADS = MAX_LICENSE_COUNT * REQUESTS_PER_FREE_LICENSE;
    constexpr int PAID_LICENSE_THREADS = MAX_LICENSE_COUNT;
    constexpr int TOTAL_THREADS = EXPLORE_THREADS + DIG_THREADS + CASH_THREADS + FREE_LICENSE_THREADS + PAID_LICENSE_THREADS;

    run_parallel(TOTAL_THREADS, [&](int thread_num)
    {
        if (thread_num < EXPLORE_THREADS)
        {
            search_treasures(client, [&](Report report)
            {
                statistics.treasures_found(report.amount);
                
                report_queue.push(report);
                
                const size_t min_queue_size = is_final_stage() ? 10 : REPORT_MIN_QUEUE_SIZE;
                if (report_queue.size() > min_queue_size)
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));

                return !client.need_stop();
            });
        }
        else if (thread_num < EXPLORE_THREADS + DIG_THREADS)
        {
            while (!client.need_stop())
            {
                const auto report = report_queue.try_pop();
                if (!report)
                {
                    if (!is_first_stage())
                        report_queue_is_empty++;

                    sleep();
                    continue;
                }

                int depth = 1;
                for (Amount digged_amount = 0; digged_amount < report->amount && depth <= MAX_DEPTH;)
                {
                    auto license_id = license_vault.use_license();
                    while (!license_id)
                    {
                        if (!is_first_stage())
                        {
                            license_vault_is_empty++;
                            if (treasure_queue.size() == 0)
                            {
                                license_vault_and_treasure_queue_is_empty++;
                                if (report_queue.size() > REPORT_MIN_QUEUE_SIZE)
                                    full_stop++;
                            }
                        }

                        sleep();
                        
                        if (client.need_stop())
                            return;

                        license_id = license_vault.use_license();
                    }

                    const auto dig_depth = depth++;
                    const auto result = client.dig(license_id.value(),
                        report->area.pos_x, report->area.pos_y, dig_depth);

                    if (!result.response)
                        break;

                    if (result.response->result() == http::status::not_found)
                        continue;

                    if (!result.value)
                        break;

                    const auto treasure_count = result.value->size();

                    statistics.treasures_digged(dig_depth, treasure_count);
                    digged_amount += treasure_count;
                    
                    for (const auto& treasure : result.value.value())
                    {
                        if (dig_depth > drop_depth)
                            treasure_queue.push({ treasure, dig_depth });
                        else
                            statistics.treasure_dropped();
                    }
                }
            }
        }
        else if (thread_num < EXPLORE_THREADS + DIG_THREADS + CASH_THREADS)
        {
            while (!client.need_stop())
            {
                const size_t min_queue_size = is_final_stage() ? 0 : TREASURE_MIN_QUEUE_SIZE;
                if (treasure_queue.size() < min_queue_size && license_vault.use_count() > 1)
                {
                    sleep();
                    continue;
                }

                const auto treasure_info = treasure_queue.try_pop();
                if (!treasure_info)
                {
                    sleep();
                    continue;
                }

                const auto result = client.cash(treasure_info->treasure);
                if (result.value)
                {
                    const Amount amount = result.value->size();
                    coin_vault.add(result.value.value());
                    statistics.treasure_sold(treasure_info->depth, amount);

                    if (game_type == 0)
                    {
                        game_type_detector.update(treasure_info->depth, amount);
                        if (const auto type = game_type_detector.game_type())
                        {
                            game_type = type.value();
                            drop_depth = DROP_LEVELS[game_type - 1];

                            info("Game type: {} (drop_depth = {})", game_type, drop_depth);
                        }
                    }
                }
            }
        }
        else if (thread_num < EXPLORE_THREADS + DIG_THREADS + CASH_THREADS + FREE_LICENSE_THREADS)
        {
            while (!client.need_stop())
            {
                if (USE_PAID_LICENSES && !is_first_stage())
                    return;

                const auto license_needed = license_vault.capacity() - license_vault.license_count();
                if (active_free_license_requests.load() / REQUESTS_PER_FREE_LICENSE >= license_needed)
                {
                    sleep();
                    continue;
                }

                ScopeIncrement<std::atomic_uint> inc(active_free_license_requests);
                
                const auto result = client.issue_license(Wallet{});
                if (result.value)
                    license_vault.add(result.value.value());
            }
        }
        else
        {
            if (!USE_PAID_LICENSES)
                return;

            while (!client.need_stop())
            {
                if (is_first_stage())
                {
                    sleep();
                    continue;
                }

                const auto license_needed = license_vault.capacity() - license_vault.license_count();
                if (active_paid_license_requests.load() >= license_needed)
                {
                    sleep();
                    continue;
                }

                ScopeIncrement<std::atomic_uint> inc(active_paid_license_requests);

                const auto wallet = coin_vault.try_take(1);

                const auto result = client.issue_license(wallet);
                if (result.value)
                    license_vault.add(result.value.value());
                else
                    coin_vault.add(wallet);
            }
        }
    });
    statistics.print_report();
    info("Unsold treasures: {}", treasure_queue.size());
    info("Report queue is empty: {}", report_queue_is_empty.load());
    info("License vault is empty: {}", license_vault_is_empty.load());
    info("License vault and treasure queue is empty: {}", license_vault_and_treasure_queue_is_empty.load());
    info("Full stop: {}", full_stop.load());
}

void algorithm_02_rng_seed_recovery(Client& client, const ProgramOptions& options)
{
    size_t memory_size = 2'000'000'000;
    char* memory = new(std::nothrow) char[memory_size];
    if (memory == nullptr)
        memory_size = 0;

    auto rng_seed_blocks = generate_rng_seed_blocks(21'691'754, memory, memory_size);

    client.wait_server_start(std::chrono::seconds(20));

    client.auto_retry.insert(http::status::too_many_requests);
    client.auto_retry.insert(http::status::service_unavailable);
    client.throw_exceptions = false;

    spdlog::stopwatch stopwatch;
    auto is_first_stage = [&]() { return stopwatch.elapsed() < std::chrono::seconds(1); };
    auto is_final_stage = [&]() { return stopwatch.elapsed() > std::chrono::seconds(LIFETIME - 2); };
    
    auto sleep = [&]() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
    
    auto handle_no_response = [&]()
    {
        if (is_final_stage() && !client.need_stop())
        {
            info("Stop client...");
            client.stop();
        }
    };

    GameTypeDetector game_type_detector;

    CoinVault coin_vault;
    LicenseVault license_vault(MAX_LICENSE_COUNT);

    ReportQueue report_queue;
    TreasureQueue treasure_queue;
    TreasurePlaceQueue treasure_place_queue;

    DigMap dig_map;

    go_rng_seed_recovery rand(1);
    std::vector<int> sold_treasure_depths;

    Statistics statistics;
    std::atomic_uint64_t report_queue_is_empty = 0;
    std::atomic_uint64_t license_vault_is_empty = 0;
    std::atomic_uint64_t license_vault_and_treasure_queue_is_empty = 0;
    std::atomic_uint64_t full_stop = 0;

    std::atomic_uint active_free_license_requests = 0;
    std::atomic_uint active_paid_license_requests = 0;
    std::atomic_uint cash_count = 0;

    std::atomic_uint game_type = 0;
    std::atomic_int drop_depth = 2;

    std::atomic_bool seed_recovered = false;
    std::atomic_bool limited_mode = true;

    constexpr size_t REPORT_MIN_QUEUE_SIZE = 50;
    constexpr size_t TREASURE_MIN_QUEUE_SIZE = 100;

    constexpr int REQUESTS_PER_FREE_LICENSE = 5;

    constexpr bool USE_PAID_LICENSES = false;

    constexpr int EXPLORE_THREADS = 1;
    constexpr int DIG_THREADS = 2;
    constexpr int CASH_THREADS = 1;
    constexpr int FREE_LICENSE_THREADS = MAX_LICENSE_COUNT * REQUESTS_PER_FREE_LICENSE;
    constexpr int PAID_LICENSE_THREADS = MAX_LICENSE_COUNT;
    constexpr int RECOVERY_THREADS = 1;
    constexpr int TOTAL_THREADS = EXPLORE_THREADS + DIG_THREADS + CASH_THREADS + FREE_LICENSE_THREADS + PAID_LICENSE_THREADS + RECOVERY_THREADS;

    run_parallel(TOTAL_THREADS, [&](int thread_num)
    {
        if (thread_num < EXPLORE_THREADS)
        {
            search_treasures(client, [&](Report report)
            {
                statistics.treasures_found(report.amount);

                report_queue.push(report);

                const size_t min_queue_size = is_final_stage() ? 10 : REPORT_MIN_QUEUE_SIZE;
                if (!limited_mode && report_queue.size() > min_queue_size)
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));

                return !client.need_stop() && !seed_recovered;
            });
        }
        else if (thread_num < EXPLORE_THREADS + DIG_THREADS)
        {
            std::optional<LicenseId> license_id;
            while (!client.need_stop())
            {
                const size_t treasure_min_queue_size = is_final_stage() ? 10 : TREASURE_MIN_QUEUE_SIZE + 10;

                if (limited_mode || treasure_queue.size() > treasure_min_queue_size)
                {
                    sleep();
                    continue;
                }

                int pos_x = 0;
                int pos_y = 0;
                Amount total_amount = 0;
                
                if (seed_recovered)
                {
                    const auto place = treasure_place_queue.try_pop();
                    if (!place)
                        return;

                    pos_x = place->pos_x;
                    pos_y = place->pos_y;
                    total_amount = place->treasures.count();
                }
                else
                {
                    const auto report = report_queue.try_pop();
                    if (!report)
                    {
                        if (!is_first_stage())
                            report_queue_is_empty++;

                        sleep();
                        continue;
                    }
                    pos_x = report->area.pos_x;
                    pos_y = report->area.pos_y;
                    total_amount = report->amount;
                }

                if (!dig_map.set(pos_x, pos_y))
                    continue;

                int depth = 1;
                for (Amount digged_amount = 0; digged_amount < total_amount && depth <= MAX_DEPTH;)
                {
                    if (!license_id)
                    {
                        license_id = license_vault.use_license();
                        while (!license_id)
                        {
                            if (!is_first_stage())
                            {
                                license_vault_is_empty++;
                                if (treasure_queue.size() == 0)
                                {
                                    license_vault_and_treasure_queue_is_empty++;
                                    if (report_queue.size() > REPORT_MIN_QUEUE_SIZE)
                                        full_stop++;
                                }
                            }

                            sleep();

                            if (client.need_stop())
                                return;

                            license_id = license_vault.use_license();
                        }
                    }

                    const auto dig_depth = depth++;
                    const auto result = client.dig(license_id.value(),
                        pos_x, pos_y, dig_depth);

                    if (!result.response)
                    {
                        handle_no_response();
                        break;
                    }

                    if (result.response->result() == http::status::not_found)
                    {
                        license_id = std::nullopt;
                        continue;
                    }

                    if (result.response->result() == http::status::forbidden)
                    {
                        license_id = std::nullopt;
                        break;
                    }

                    if (!result.value)
                        break;

                    license_id = std::nullopt;

                    const auto treasure_count = result.value->size();

                    statistics.treasures_digged(dig_depth, treasure_count);
                    digged_amount += treasure_count;

                    for (const auto& treasure : result.value.value())
                    {
                        if (dig_depth > drop_depth)
                            treasure_queue.push({ treasure, dig_depth });
                        else
                            statistics.treasure_dropped();
                    }
                }
            }
        }
        else if (thread_num < EXPLORE_THREADS + DIG_THREADS + CASH_THREADS)
        {
            while (!client.need_stop())
            {
                const size_t min_queue_size = is_final_stage() ? 0 : TREASURE_MIN_QUEUE_SIZE;
                if (treasure_queue.size() < min_queue_size && license_vault.use_count() > 1)
                {
                    sleep();
                    continue;
                }

                if (seed_recovered && !sold_treasure_depths.empty())
                {
                    for (int depth : sold_treasure_depths)
                        rand.int31n(1 + TREASURE_COST_RANGES[game_type - 1][depth - 1].delta());

                    sold_treasure_depths.clear();
                }

                const auto treasure_info = seed_recovered
                    ? get_optimal_treasure(treasure_queue, game_type, rand)
                    : treasure_queue.try_pop();
                if (!treasure_info)
                {
                    sleep();
                    continue;
                }

                const auto result = client.cash(treasure_info->treasure);
                if (result.value)
                {
                    const Amount amount = result.value->size();
                    coin_vault.add(result.value.value());
                    statistics.treasure_sold(treasure_info->depth, amount);
                    sold_treasure_depths.push_back(treasure_info->depth);

                    if (game_type == 0)
                    {
                        game_type_detector.update(treasure_info->depth, amount);
                        if (const auto type = game_type_detector.game_type())
                        {
                            game_type = type.value();
                            drop_depth = DROP_LEVELS[game_type - 1];

                            info("Game type: {} (drop_depth = {})", game_type, drop_depth);
                        }
                    }
                }
                if (!result.response)
                    handle_no_response();
            }
        }
        else if (thread_num < EXPLORE_THREADS + DIG_THREADS + CASH_THREADS + FREE_LICENSE_THREADS)
        {
            while (!client.need_stop())
            {
                if (USE_PAID_LICENSES && !is_first_stage())
                    return;

                const auto license_needed = license_vault.capacity() - license_vault.license_count();
                if (limited_mode || active_free_license_requests.load() / REQUESTS_PER_FREE_LICENSE >= license_needed)
                {
                    sleep();
                    continue;
                }

                const ScopeIncrement<std::atomic_uint> inc(active_free_license_requests);

                const auto result = client.issue_license(Wallet{});
                if (result.value)
                    license_vault.add(result.value.value());
                
                if (!result.response)
                    handle_no_response();
            }
        }
        else if (thread_num < EXPLORE_THREADS + DIG_THREADS + CASH_THREADS + FREE_LICENSE_THREADS + PAID_LICENSE_THREADS)
        {
            if (!USE_PAID_LICENSES)
                return;

            while (!client.need_stop())
            {
                if (is_first_stage())
                {
                    sleep();
                    continue;
                }

                const auto license_needed = license_vault.capacity() - license_vault.license_count();
                if (limited_mode || active_paid_license_requests.load() >= license_needed)
                {
                    sleep();
                    continue;
                }

                const ScopeIncrement<std::atomic_uint> inc(active_paid_license_requests);

                const auto wallet = coin_vault.try_take(1);

                const auto result = client.issue_license(wallet);
                if (result.value)
                    license_vault.add(result.value.value());
                else
                    coin_vault.add(wallet);

                if (!result.response)
                    handle_no_response();
            }
        }
        else
        {
            const auto signature = get_rng_signature(client, license_vault,
                active_free_license_requests, limited_mode);

            info("Signature: {} {}", signature.first.to_ullong(), signature.second.to_ullong());

            while (game_type == 0)
            {
                if (client.need_stop())
                    return;

                sleep();
            }

            const auto raw_seed = find_raw_seed(signature, game_type - 1, rng_seed_blocks);
            
            delete[] memory;
            memory = nullptr;

            rng_seed_blocks.clear();
            rng_seed_blocks.shrink_to_fit();

            if (raw_seed == 0)
                return;

            info("Raw: {}", raw_seed);
            info("Current balance: {}", coin_vault.count());

            if (!check_raw_seed(client, raw_seed))
                return;

            //const auto treasure_count = find_treasure_count(client, dig_map, raw_seed);
            //info("Treasure count: {}", treasure_count);

            rand.set_state(raw_seed);

            TreasureMap treasure_map;
            fill_treasure_map(treasure_map, rand, TREASURE_COUNT);

            auto treasure_places = find_treasure_places(treasure_map);
            treasure_places = order_treasure_places(treasure_places, game_type);
            treasure_place_queue.push_all(treasure_places);

            seed_recovered = true;

            info("Seed recovered");
        }
    });
    statistics.print_report();
    info("Unsold treasures: {}", treasure_queue.size());
    info("Report queue is empty: {}", report_queue_is_empty.load());
    info("License vault is empty: {}", license_vault_is_empty.load());
    info("License vault and treasure queue is empty: {}", license_vault_and_treasure_queue_is_empty.load());
    info("Full stop: {}", full_stop.load());
}

static const std::vector<std::function<void(Client&, const ProgramOptions&)>> client_algorithms =
{
    algorithm_01_basic,
    algorithm_02_rng_seed_recovery,
};

void dump_environment_info()
{
#if __linux__
    {
        utsname data = {};
        if (uname(&data) == 0)
        {
            info("uname: {} | {} | {} | {}", data.sysname, data.release, data.version, data.machine);
        }
    }
#endif

    {
        cpu_raw_data_t raw = {};
        cpu_id_t id = {};
        if (cpuid_get_raw_data(&raw) == 0 && cpu_identify(&raw, &id) == 0)
        {
            info("cpuid: {}", id.brand_str);
        }
        else
        {
            warn("Processor cannot be determined");
        }
    }

    for (const auto& parameter : boost::this_process::environment())
    {
        info("env: {}={}", parameter.get_name(), parameter.to_string());
    }
}

void dump_metrics(const Client& client)
{
    using boost::algorithm::trim_copy;

    const double total_elapsed = client.stopwatch.elapsed().count();
    std::uint64_t total_count = 0;
    std::uint64_t success_count = 0;
    std::uint64_t error_count = 0;
    std::uint64_t exception_count = 0;

    auto format_timings = [](const Client::Timings& timings)
    {
        return fmt::format("min_time={:.3f} avg_time={:.3f} max_time={:.3f}",
            timings.min_time * 1000,
            timings.avg_time() * 1000,
            timings.max_time * 1000);
    };

    auto rps = [&](std::uint64_t count, double elapsed)
    {
        return fmt::format("{:.1f}", elapsed > 0 ? count / elapsed : 0.);
    };

    const auto metrics = client.get_metrics();
    for (const auto& [id, request_metrics] : metrics.requests)
    {
        const double elapsed = request_metrics.elapsed();
        total_count += request_metrics.total_count;
        success_count += request_metrics.success_count;
        error_count += request_metrics.error_count;
        exception_count += request_metrics.exception_count;

        std::string message = fmt::format("Metrics for `{}`:", id);
        
        if (request_metrics.exception_count > 0)
        {
            message += fmt::format("\n|-Exceptions: count={} rate={} {}",
                request_metrics.exception_count,
                rps(request_metrics.exception_count, elapsed),
                format_timings(request_metrics.exception_timings));

            for (const auto& [error_message, count] : request_metrics.exceptions)
            {
                message += fmt::format("\n|  -Exception: {} [count={} rate={}]",
                    trim_copy(error_message), count, rps(count, elapsed));
            }
        }

        if (request_metrics.error_count > 0)
        {
            message += fmt::format("\n|-Errors: count={} rate={} {}",
                request_metrics.error_count,
                rps(request_metrics.error_count, elapsed),
                format_timings(request_metrics.error_timinigs));

            for (const auto& [status, error_metrics] : request_metrics.errors)
            {
                message += fmt::format(
                    "\n|  -Error {}: count={} rate={} {}",
                    static_cast<unsigned>(status),
                    error_metrics.total_count,
                    rps(error_metrics.total_count, elapsed),
                    format_timings(error_metrics.timings));

                for (const auto& [body, count] : error_metrics.messages)
                {
                    message += fmt::format("\n|    -Data: {} [count={} rate={}]",
                        trim_copy(body), count, rps(count, elapsed));
                }
            }
        }

        if (request_metrics.success_count > 0)
        {
            message += fmt::format("\n|-Success: count={} rate={} {}",
                request_metrics.success_count,
                rps(request_metrics.success_count, elapsed),
                format_timings(request_metrics.success_timings));
        }

        message += fmt::format("\n|-Total: count={} rate={} {}",
            request_metrics.total_count,
            rps(request_metrics.total_count, elapsed),
            format_timings(request_metrics.total_timings));
        
        info(message);
    }

    if (!metrics.dig.levels.empty())
    {
        info("Dig levels: {}", join_values(metrics.dig.levels, ", "));
    }

    if (!metrics.cash.treasure_cost.empty())
    {
        info("Treasure cost: {}", join_values(metrics.cash.treasure_cost, ", "));
    }

    info("Summary: request_rate={} success_rate={} error_rate={} exception_rate={} request_count={} time={:.3f}",
        rps(total_count, total_elapsed),
        rps(success_count, total_elapsed),
        rps(error_count, total_elapsed),
        rps(exception_count, total_elapsed),
        total_count, total_elapsed);
}

void run_event_loop(boost::asio::io_context& ioc)
{
    try
    {
        info("Run event loop...");
        ioc.run();
        info("Event loop stopped");
    }
    catch (...)
    {
        spdlog::critical("Exception in event loop: " + current_exception_info());
    }
}

void run_client(const ProgramOptions& options)
{
    //dump_environment_info();

    boost::asio::io_context ioc{ 2 };

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code, int signal_number)
    {
        info("Receive {}, stop client...", signal_number == SIGINT ? "SIGINT" : "SIGTERM");
        ioc.stop();
    });

    boost::asio::deadline_timer lifetime_timer(ioc);
    if (options.lifetime != 0)
    {
        info("Limit client lifetime to {} sec", options.lifetime);
        lifetime_timer.expires_from_now(boost::posix_time::seconds(options.lifetime));
        lifetime_timer.async_wait([&](boost::system::error_code)
        {
            info("Client lifetime is over, stop client...");
            ioc.stop();
        });
    }

    info("Resolve host \"{}\"...", options.address);
    const auto host = ares::resolve_host(options.address, AF_INET);
    info("Resolved host: {}", host.to_string());

    const auto endpoint = tcp::endpoint(
        boost::asio::ip::make_address(host.addresses.at(0)),
        options.port);

    Client client(ioc, endpoint, options.address);
    client.rps_limiter.set_limit(options.rps_limit);
    client.explore_rps_limiter.set_limit(options.explore_rps_limit);
    client.license_rps_limiter.set_limit(options.license_rps_limit);

    std::thread thread([&]()
    {
        try
        {
            info("Run algorithm...");
            client_algorithms.at(options.algorithm_id - 1)(client, options);
        }
        catch (...)
        {
            spdlog::critical("Exception in algorithm: " + current_exception_info());
        }
        ioc.stop();
    });

    run_event_loop(ioc);

    info("Wait until all threads exit...");
    thread.join();

    dump_metrics(client);
    
    info("Gracefully stopped");
}

void initialize_logging(const ProgramOptions& options)
{
    std::shared_ptr<spdlog::sinks::sink> default_log_sink;

    if (options.no_color)
    {
        default_log_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    }
    else
    {
        default_log_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    }

    default_log_sink->set_pattern("[%T.%f] [%^%5l%$] [%5t] %v");

    spdlog::init_thread_pool(16384, 1);

    auto default_logger = std::make_shared<spdlog::async_logger>("default", default_log_sink, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    default_logger->set_level(options.log_level);

    spdlog::set_default_logger(default_logger);
}

void configure_app(CLI::App& app, ProgramOptions& options)
{
    static const std::vector<std::pair<std::string, spdlog::level::level_enum>> log_levels =
    {
        { "trace", spdlog::level::trace },
        { "debug", spdlog::level::debug },
        { "info", spdlog::level::info },
        { "warning", spdlog::level::warn },
        { "error", spdlog::level::err },
        { "critical", spdlog::level::critical },
        { "off", spdlog::level::off },
    };

    app.description("Client for the \"Gold Rush\" contest (https://cups.mail.ru/ru/contests/goldrush)");

    app.set_config("-c,--config", "goldrush-client.conf", "Configuration file for goldrush-client");

    app.add_option("--address", options.address)
        ->group("Server Options")
        ->envname("ADDRESS")
        ->description("HTTP server address")
        ->required();

    app.add_option("--port", options.port)
        ->group("Server Options")
        ->description("TCP port number")
        ->type_name("PORT")
        ->capture_default_str()
        ->check(CLI::Range(0, 0xFFFF));

    app.add_option("--algorithm-id", options.algorithm_id)
        ->group("Client Options")
        ->description("Client algorithm identifier")
        ->capture_default_str()
        ->check(CLI::Range(size_t(1), client_algorithms.size()));

    app.add_option("--lifetime", options.lifetime)
        ->group("Client Options")
        ->description("Client lifetime in seconds (special value `0` for unlimited lifetime)")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);

    app.add_option("--score-limit", options.score_limit)
        ->group("Client Options")
        ->description("Score limit (special value `0` for unlimited score)")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);

    app.add_option("--rps-limit", options.rps_limit)
        ->group("Client Options")
        ->description("RPS limit (special value `0` for unlimited RPS)")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);

    app.add_option("--explore-rps-limit", options.explore_rps_limit)
        ->group("Client Options")
        ->description("'POST /explore' RPS limit (special value `0` for unlimited RPS)")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);

    app.add_option("--license-rps-limit", options.license_rps_limit)
        ->group("Client Options")
        ->description("'POST /license' RPS limit (special value `0` for unlimited RPS)")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);

    app.add_option("--log-level", options.log_level)
        ->group("Logging Options")
        ->description("Logging level (default: trace)")
        ->transform(CLI::CheckedTransformer(log_levels));

    app.add_flag("--no-color", options.no_color)
        ->group("Logging Options")
        ->description("Disables the colored output");
}

int main(int argc, char* argv[])
{
    ProgramOptions options;
    try
    {
        CLI::App app;
        configure_app(app, options);

        try
        {
#ifdef APP_OPTIONS
            app.parse(APP_OPTIONS);
#else
            app.parse(argc, argv);
#endif
            options.no_color = true;
        }
        catch (const CLI::ParseError& e)
        {
            return app.exit(e);
        }
    }
    catch (...)
    {
        std::cerr << "Failed to parse program options: " << current_exception_info();
        return EXIT_FAILURE;
    }

    try
    {
        initialize_logging(options);
    }
    catch (...)
    {
        std::cerr << "Failed to initialize logging: " << current_exception_info();
        return EXIT_FAILURE;
    }

    try
    {
        ares::library_init();

        run_client(options);
        
        ares::library_cleanup();
        spdlog::shutdown();
        return EXIT_SUCCESS;
    }
    catch (...)
    {
        spdlog::critical("Exception in main(): " + current_exception_info());
        spdlog::shutdown();
        return EXIT_FAILURE;
    }
}
