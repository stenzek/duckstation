#include "android_http_downloader.h"
#include "android_host_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"
#include <algorithm>
#include <functional>
Log_SetChannel(AndroidHTTPDownloader);

namespace FrontendCommon {

AndroidHTTPDownloader::AndroidHTTPDownloader() : HTTPDownloader() {}

AndroidHTTPDownloader::~AndroidHTTPDownloader()
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  if (m_URLDownloader_class)
    env->DeleteGlobalRef(m_URLDownloader_class);
}

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create()
{
  std::unique_ptr<AndroidHTTPDownloader> instance(std::make_unique<AndroidHTTPDownloader>());
  if (!instance->Initialize())
    return {};

  return instance;
}

bool AndroidHTTPDownloader::Initialize()
{
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jclass klass = env->FindClass("com/github/stenzek/duckstation/URLDownloader");
  if (!klass)
    return false;

  m_URLDownloader_class = static_cast<jclass>(env->NewGlobalRef(klass));
  if (!m_URLDownloader_class)
    return false;

  m_URLDownloader_constructor = env->GetMethodID(klass, "<init>", "()V");
  m_URLDownloader_get = env->GetMethodID(klass, "get", "(Ljava/lang/String;)Z");
  m_URLDownloader_post = env->GetMethodID(klass, "post", "(Ljava/lang/String;[B)Z");
  m_URLDownloader_getStatusCode = env->GetMethodID(klass, "getStatusCode", "()I");
  m_URLDownloader_getData = env->GetMethodID(klass, "getData", "()[B");
  if (!m_URLDownloader_constructor || !m_URLDownloader_get || !m_URLDownloader_post || !m_URLDownloader_getStatusCode ||
      !m_URLDownloader_getData)
  {
    return false;
  }

  m_thread_pool = std::make_unique<cb::ThreadPool>(m_max_active_requests);
  return true;
}

void AndroidHTTPDownloader::ProcessRequest(Request* req)
{
  std::unique_lock<std::mutex> cancel_lock(m_cancel_mutex);
  if (req->closed.load())
    return;

  cancel_lock.unlock();
  req->status_code = -1;
  req->start_time = Common::Timer::GetValue();

  // TODO: Move to Java side...
  JNIEnv* env;
  if (AndroidHelpers::GetJavaVM()->AttachCurrentThread(&env, nullptr) == JNI_OK)
  {
    jobject obj = env->NewObject(m_URLDownloader_class, m_URLDownloader_constructor);
    jstring url_string = env->NewStringUTF(req->url.c_str());
    jboolean result;
    if (req->post_data.empty())
    {
      result = env->CallBooleanMethod(obj, m_URLDownloader_get, url_string);
    }
    else
    {
      jbyteArray post_data = env->NewByteArray(static_cast<jsize>(req->post_data.size()));
      env->SetByteArrayRegion(post_data, 0, static_cast<jsize>(req->post_data.size()),
                              reinterpret_cast<const jbyte*>(req->post_data.data()));
      result = env->CallBooleanMethod(obj, m_URLDownloader_post, url_string, post_data);
      env->DeleteLocalRef(post_data);
    }

    env->DeleteLocalRef(url_string);

    if (result)
    {
      req->status_code = env->CallIntMethod(obj, m_URLDownloader_getStatusCode);

      jbyteArray data = reinterpret_cast<jbyteArray>(env->CallObjectMethod(obj, m_URLDownloader_getData));
      if (data)
      {
        const u32 size = static_cast<u32>(env->GetArrayLength(data));
        req->data.resize(size);
        if (size > 0)
        {
          jbyte* data_ptr = env->GetByteArrayElements(data, nullptr);
          std::memcpy(req->data.data(), data_ptr, size);
          env->ReleaseByteArrayElements(data, data_ptr, 0);
        }

        env->DeleteLocalRef(data);
      }

      Log_DevPrintf("Request for '%s' returned status code %d and %zu bytes", req->url.c_str(), req->status_code,
                    req->data.size());
    }
    else
    {
      Log_ErrorPrintf("Request for '%s' failed", req->url.c_str());
    }

    env->DeleteLocalRef(obj);
    AndroidHelpers::GetJavaVM()->DetachCurrentThread();
  }
  else
  {
    Log_ErrorPrintf("AttachCurrentThread() failed");
  }

  cancel_lock.lock();
  req->state = Request::State::Complete;
  if (req->closed.load())
    delete req;
  else
    req->closed.store(true);
}

HTTPDownloader::Request* AndroidHTTPDownloader::InternalCreateRequest()
{
  Request* req = new Request();
  return req;
}

void AndroidHTTPDownloader::InternalPollRequests()
{
  // noop - uses thread pool
}

bool AndroidHTTPDownloader::StartRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);
  Log_DevPrintf("Started HTTP request for '%s'", req->url.c_str());
  req->state = Request::State::Started;
  req->start_time = Common::Timer::GetValue();
  m_thread_pool->Schedule(std::bind(&AndroidHTTPDownloader::ProcessRequest, this, req));
  return true;
}

void AndroidHTTPDownloader::CloseRequest(HTTPDownloader::Request* request)
{
  std::unique_lock<std::mutex> cancel_lock(m_cancel_mutex);
  Request* req = static_cast<Request*>(request);
  if (req->closed.load())
    delete req;
  else
    req->closed.store(true);
}

} // namespace FrontendCommon
