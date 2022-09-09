#include "http_downloader.h"
#include "assert.h"
#include "log.h"
#include "timer.h"
Log_SetChannel(HTTPDownloader);

static constexpr float DEFAULT_TIMEOUT_IN_SECONDS = 30;
static constexpr u32 DEFAULT_MAX_ACTIVE_REQUESTS = 4;

namespace Common {

const char HTTPDownloader::DEFAULT_USER_AGENT[] =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:85.0) Gecko/20100101 Firefox/85.0";

HTTPDownloader::HTTPDownloader()
  : m_timeout(DEFAULT_TIMEOUT_IN_SECONDS), m_max_active_requests(DEFAULT_MAX_ACTIVE_REQUESTS)
{
}

HTTPDownloader::~HTTPDownloader() = default;

void HTTPDownloader::SetTimeout(float timeout)
{
  m_timeout = timeout;
}

void HTTPDownloader::SetMaxActiveRequests(u32 max_active_requests)
{
  Assert(max_active_requests > 0);
  m_max_active_requests = max_active_requests;
}

void HTTPDownloader::CreateRequest(std::string url, Request::Callback callback)
{
  Request* req = InternalCreateRequest();
  req->parent = this;
  req->type = Request::Type::Get;
  req->url = std::move(url);
  req->callback = std::move(callback);
  req->start_time = Common::Timer::GetCurrentValue();

  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  if (LockedGetActiveRequestCount() < m_max_active_requests)
  {
    if (!StartRequest(req))
      return;
  }

  LockedAddRequest(req);
}

void HTTPDownloader::CreatePostRequest(std::string url, std::string post_data, Request::Callback callback)
{
  Request* req = InternalCreateRequest();
  req->parent = this;
  req->type = Request::Type::Post;
  req->url = std::move(url);
  req->post_data = std::move(post_data);
  req->callback = std::move(callback);
  req->start_time = Common::Timer::GetCurrentValue();

  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  if (LockedGetActiveRequestCount() < m_max_active_requests)
  {
    if (!StartRequest(req))
      return;
  }

  LockedAddRequest(req);
}

void HTTPDownloader::LockedPollRequests(std::unique_lock<std::mutex>& lock)
{
  if (m_pending_http_requests.empty())
    return;

  InternalPollRequests();

  const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
  u32 active_requests = 0;
  u32 unstarted_requests = 0;

  for (size_t index = 0; index < m_pending_http_requests.size();)
  {
    Request* req = m_pending_http_requests[index];
    if (req->state == Request::State::Pending)
    {
      unstarted_requests++;
      index++;
      continue;
    }

    if (req->state == Request::State::Started && current_time >= req->start_time &&
        Common::Timer::ConvertValueToSeconds(current_time - req->start_time) >= m_timeout)
    {
      // request timed out
      Log_ErrorPrintf("Request for '%s' timed out", req->url.c_str());

      req->state.store(Request::State::Cancelled);
      m_pending_http_requests.erase(m_pending_http_requests.begin() + index);
      lock.unlock();

      req->callback(-1, Request::Data());

      CloseRequest(req);

      lock.lock();
      continue;
    }

    if (req->state != Request::State::Complete)
    {
      active_requests++;
      index++;
      continue;
    }

    // request complete
    Log_VerbosePrintf("Request for '%s' complete, returned status code %u and %zu bytes", req->url.c_str(),
                      req->status_code, req->data.size());
    m_pending_http_requests.erase(m_pending_http_requests.begin() + index);

    // run callback with lock unheld
    lock.unlock();
    req->callback(req->status_code, req->data);
    CloseRequest(req);
    lock.lock();
  }

  // start new requests when we finished some
  if (unstarted_requests > 0 && active_requests < m_max_active_requests)
  {
    for (size_t index = 0; index < m_pending_http_requests.size();)
    {
      Request* req = m_pending_http_requests[index];
      if (req->state != Request::State::Pending)
      {
        index++;
        continue;
      }

      if (!StartRequest(req))
      {
        m_pending_http_requests.erase(m_pending_http_requests.begin() + index);
        continue;
      }

      active_requests++;
      index++;

      if (active_requests >= m_max_active_requests)
        break;
    }
  }
}

void HTTPDownloader::PollRequests()
{
  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  LockedPollRequests(lock);
}

void HTTPDownloader::WaitForAllRequests()
{
  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  while (!m_pending_http_requests.empty())
    LockedPollRequests(lock);
}

void HTTPDownloader::LockedAddRequest(Request* request)
{
  m_pending_http_requests.push_back(request);
}

u32 HTTPDownloader::LockedGetActiveRequestCount()
{
  u32 count = 0;
  for (Request* req : m_pending_http_requests)
  {
    if (req->state == Request::State::Started || req->state == Request::State::Receiving)
      count++;
  }
  return count;
}

std::string HTTPDownloader::URLEncode(const std::string_view& str)
{
  std::string ret;
  ret.reserve(str.length() + ((str.length() + 3) / 4) * 3);

  for (size_t i = 0, l = str.size(); i < l; i++)
  {
    const char c = str[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
        c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')')
    {
      ret.push_back(c);
    }
    else
    {
      ret.push_back('%');

      const unsigned char n1 = static_cast<unsigned char>(c) >> 4;
      const unsigned char n2 = static_cast<unsigned char>(c) & 0x0F;
      ret.push_back((n1 >= 10) ? ('a' + (n1 - 10)) : ('0' + n1));
      ret.push_back((n2 >= 10) ? ('a' + (n2 - 10)) : ('0' + n2));
    }
  }

  return ret;
}

std::string HTTPDownloader::URLDecode(const std::string_view& str)
{
  std::string ret;
  ret.reserve(str.length());

  for (size_t i = 0, l = str.size(); i < l; i++)
  {
    const char c = str[i];
    if (c == '+')
    {
      ret.push_back(c);
    }
    else if (c == '%')
    {
      if ((i + 2) >= str.length())
        break;

      const char clower = str[i + 1];
      const char cupper = str[i + 2];
      const unsigned char lower =
        (clower >= '0' && clower <= '9') ?
          static_cast<unsigned char>(clower - '0') :
          ((clower >= 'a' && clower <= 'f') ?
             static_cast<unsigned char>(clower - 'a') :
             ((clower >= 'A' && clower <= 'F') ? static_cast<unsigned char>(clower - 'A') : 0));
      const unsigned char upper =
        (cupper >= '0' && cupper <= '9') ?
          static_cast<unsigned char>(cupper - '0') :
          ((cupper >= 'a' && cupper <= 'f') ?
             static_cast<unsigned char>(cupper - 'a') :
             ((cupper >= 'A' && cupper <= 'F') ? static_cast<unsigned char>(cupper - 'A') : 0));
      const char dch = static_cast<char>(lower | (upper << 4));
      ret.push_back(dch);
    }
    else
    {
      ret.push_back(c);
    }
  }

  return std::string(str);
}

} // namespace Common