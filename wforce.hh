#pragma once
#include "ext/luawrapper/include/LuaContext.hpp"
#include <time.h>
#include "misc.hh"
#include "iputils.hh"
#include <atomic>
#include <boost/circular_buffer.hpp>
#include <mutex>
#include <thread>
#include "sholder.hh"

struct DNSDistStats
{
  using stat_t=std::atomic<uint64_t>;
  stat_t responses{0};
  stat_t servfailResponses{0};
  stat_t queries{0};
  stat_t aclDrops{0};
  stat_t blockFilter{0};
  stat_t ruleDrop{0};
  stat_t ruleNXDomain{0};
  stat_t selfAnswered{0};
  stat_t downstreamTimeouts{0};
  stat_t downstreamSendErrors{0};
  double latency{0};
  
};

extern struct DNSDistStats g_stats;


struct StopWatch
{
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
  struct timespec d_start{0,0};
  void start() {  
    if(clock_gettime(CLOCK_MONOTONIC_RAW, &d_start) < 0)
      unixDie("Getting timestamp");
    
  }
  
  double udiff() const {
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC_RAW, &now) < 0)
      unixDie("Getting timestamp");
    
    return 1000000.0*(now.tv_sec - d_start.tv_sec) + (now.tv_nsec - d_start.tv_nsec)/1000.0;
  }

  double udiffAndSet() {
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC_RAW, &now) < 0)
      unixDie("Getting timestamp");
    
    auto ret= 1000000.0*(now.tv_sec - d_start.tv_sec) + (now.tv_nsec - d_start.tv_nsec)/1000.0;
    d_start = now;
    return ret;
  }

};

class QPSLimiter
{
public:
  QPSLimiter()
  {
  }

  QPSLimiter(unsigned int rate, unsigned int burst) : d_rate(rate), d_burst(burst), d_tokens(burst)
  {
    d_passthrough=false;
    d_prev.start();
  }

  unsigned int getRate() const
  {
    return d_passthrough? 0 : d_rate;
  }

  int getPassed() const
  {
    return d_passed;
  }
  int getBlocked() const
  {
    return d_blocked;
  }

  bool check() const // this is not quite fair
  {
    if(d_passthrough)
      return true;
    auto delta = d_prev.udiffAndSet();
  
    d_tokens += 1.0*d_rate * (delta/1000000.0);

    if(d_tokens > d_burst)
      d_tokens = d_burst;

    bool ret=false;
    if(d_tokens >= 1.0) { // we need this because burst=1 is weird otherwise
      ret=true;
      --d_tokens;
      d_passed++;
    }
    else
      d_blocked++;

    return ret; 
  }
private:
  bool d_passthrough{true};
  unsigned int d_rate;
  unsigned int d_burst;
  mutable double d_tokens;
  mutable StopWatch d_prev;
  mutable unsigned int d_passed{0};
  mutable unsigned int d_blocked{0};
};



struct Rings {
  Rings()
  {
    clientRing.set_capacity(10000);
    
  }
  boost::circular_buffer<ComboAddress> clientRing;
};

extern Rings g_rings; // XXX locking for this is still substandard, queryRing and clientRing need RW lock

struct ClientState
{
  ComboAddress local;
  int udpFD;
  int tcpFD;
};

class TCPClientCollection {
  std::vector<int> d_tcpclientthreads;
  std::atomic<uint64_t> d_pos;
public:
  std::atomic<uint64_t> d_queued, d_numthreads;

  TCPClientCollection()
  {
    d_tcpclientthreads.reserve(1024);
  }

  int getThread() 
  {
    int pos = d_pos++;
    ++d_queued;
    return d_tcpclientthreads[pos % d_numthreads];
  }
  void addTCPClientThread();
};

extern TCPClientCollection g_tcpclientthreads;

extern std::mutex g_luamutex;
extern LuaContext g_lua;
extern std::string g_outputBuffer; // locking for this is ok, as locked by g_luamutex


extern GlobalStateHolder<NetmaskGroup> g_ACL;

extern ComboAddress g_serverControl; // not changed during runtime

extern std::vector<ComboAddress> g_locals; // not changed at runtime
extern std::string g_key; // in theory needs locking

struct dnsheader;

void controlThread(int fd, ComboAddress local);
vector<std::function<void(void)>> setupLua(bool client, const std::string& config);
template<typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args)
{
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
void dnsdistWebserverThread(int sock, const ComboAddress& local, const string& password);
bool getMsgLen(int fd, uint16_t* len);
bool putMsgLen(int fd, uint16_t len);
void* tcpAcceptorThread(void* p);

struct LoginTuple
{
  time_t t;
  ComboAddress remote;
  string login;
  string pwhash;
  bool success;
};

extern std::vector<LoginTuple> g_logins;

class WForceDB
{
public:
  WForceDB() {}
  WForceDB(const WForceDB&) = delete;
  void reportTuple(const LoginTuple& lp);

  int countFailures(const ComboAddress& remote, int seconds) const;
  int countDiffFailures(const ComboAddress& remote, int seconds) const;
  int countDiffFailures(const ComboAddress& remote, string login, int seconds) const;
  std::vector<LoginTuple> getTuples() const;
private:
  std::vector<LoginTuple> d_logins;
  mutable std::mutex d_mutex;
};

int allowTupleDefault(const WForceDB* wfd, const LoginTuple& lp);
extern WForceDB g_wfdb;
typedef std::function<int(const WForceDB*, const LoginTuple&)> allow_t;
extern allow_t g_allow;
