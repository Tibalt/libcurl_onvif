#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/timerfd.h>
#include <signal.h>
#include <string>

#include <syslog.h>

#include <curl/curl.h>

#define HANDLECOUNT 128
//#define CLOCKID CLOCK_REALTIME
#define CLOCKID CLOCK_MONOTONIC
#define SIG SIGRTMIN

#define errExit(msg)    \
  do                    \
  {                     \
    perror(msg);        \
    exit(EXIT_FAILURE); \
  } while (0)

/* holder for curl fetch */
struct curl_fetch_st
{
  int id;
  char *payload;
  size_t size;
};

/* callback for curl fetch */
size_t curl_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;                          /* calculate buffer size */
  struct curl_fetch_st *p = (struct curl_fetch_st *)userp; /* cast pointer to fetch struct */

  /* expand buffer using a temporary pointer to avoid memory leaks */
  // todo: expand only when the image is bigger than 500KB
  //       refuse too big image, more than 1MB
  void *temp = realloc((void *)(p->payload), p->size + realsize + 1);
  /* check allocation */
  if (temp == NULL)
  {
    /* this isn't good */
    printf("ERROR: Failed to expand buffer in curl_callback");
    /* free buffer */
    free(p->payload);
    /* return */
    return 1;
  }
  /* assign payload */
  p->payload = (char *)temp;

  /* copy contents to buffer */
  memcpy(&(p->payload[p->size]), contents, realsize);

  /* set new buffer size */
  p->size += realsize;

  /* ensure null termination */
  p->payload[p->size] = 0;

  /* return size */
  return realsize;
}
/* fetch and return url body via curl */
int curl_set_url(CURL *ch, const char *url, struct curl_fetch_st *fetch)
{

  /* init payload */
  // memset(fetch->payload,0,IMAGELIMIT);
  fetch->size = 0;

  /* init size */
  fetch->size = 0;

  /* set url to fetch */
  curl_easy_setopt(ch, CURLOPT_URL, url);

  /* set calback function */
  curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, curl_callback);

  /* pass fetch struct pointer */
  curl_easy_setopt(ch, CURLOPT_WRITEDATA, (void *)fetch);

  /* store a pointer to our private struct */
  curl_easy_setopt(ch, CURLOPT_PRIVATE, fetch);

  /* set default user agent */
  curl_easy_setopt(ch, CURLOPT_USERAGENT, "libcurl-agent/1.0");

  /* set timeout */
  curl_easy_setopt(ch, CURLOPT_TIMEOUT, 15);

  /* enable location redirects */
  curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1);

  /* set maximum allowed redirects */
  curl_easy_setopt(ch, CURLOPT_MAXREDIRS, 1);

  /* fetch the url */
  // rcode = curl_easy_perform(ch);

  /* return */
  return 0;
}
int main(int argc, char *argv[])
{

  openlog("dispatcher", LOG_PID | LOG_CONS, LOG_LOCAL3);
  syslog(LOG_ERR, "image dispatcher begins!");

  int fd = timerfd_create(CLOCKID, 0);
  if (fd == -1)
  {
    perror("timer fd create failed:");
    return 1;
  }

  CURL *handles[HANDLECOUNT];
  std::string urls[HANDLECOUNT];

  CURLM *multi_handle = 0;
  CURLcode rcode; /* curl result code */

  struct curl_fetch_st curl_fetch[HANDLECOUNT]; /* curl fetch struct */

  for (int i = 0; i < HANDLECOUNT; i++)
  {
    urls[i] = "http://192.168.20.181/onvif-http/snapshot?Profile_1";
    // urls[i] = "http://192.168.2.181/onvif-http/snapshot?Profile_1";
    if ((handles[i] = curl_easy_init()) == NULL)
    {
      syslog(LOG_ERR, "Failed to create easy curl handle");
      return 1;
    }
    curl_fetch[i].id = i;
    curl_fetch[i].size = 0;
    curl_fetch[i].payload = NULL;
    curl_set_url(handles[i], urls[i].c_str(), &curl_fetch[i]);
  }

  int still_running, ready_fd, msgs_left;
  multi_handle = curl_multi_init();

  while (1)
  {
    for (int i = 0; i < HANDLECOUNT; i++)
    {
      CURLMcode mc = curl_multi_add_handle(multi_handle, handles[i]);
      if (mc != 0)
      {
        syslog(LOG_WARNING, "[camera %d]add to multi handle failed: %s!",
               i, curl_multi_strerror(mc));
        double total = 0;
        CURLcode c = curl_easy_getinfo(handles[i],
                                       CURLINFO_TOTAL_TIME, &total);
        if (CURLE_OK == c)
        {
          syslog(LOG_WARNING, "[camera %d]time consume: %.1f", i, total);
        }
        else
          syslog(LOG_ERR, "curl_easy_getinfo CURLINFO_TOTAL_TIME:%s",
                 curl_easy_strerror(c));
      }
      else
      {
        syslog(LOG_DEBUG, "[camera %d]add to multi handle OK!", i);
      }
    }

    struct timespec now;
    if (clock_gettime(CLOCKID, &now) == -1)
    {
      perror("clock_gettime");
      return 1;
    }

    /* Initial expiration */
    struct itimerspec its;
    its.it_value.tv_sec = now.tv_sec + 1;
    its.it_value.tv_nsec = now.tv_nsec;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;

    /*start timer after everything initialized */
    /*exp shows the accuracy is better with TFD_TIMER_ABSTIME*/
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) == -1)
    {
      perror("timer set failed:");
      return 1;
    }

    syslog(LOG_INFO, "arm the timer ");

    struct curl_waitfd poll_timer_fd;
    poll_timer_fd.fd = fd;
    poll_timer_fd.events = CURL_WAIT_POLLIN;

    CURLMcode mc = curl_multi_perform(multi_handle, &still_running);
    do
    {
      if (mc)
      {
        syslog(LOG_ERR, "curl_multi_perform:%s,still running:%d",
               curl_multi_strerror(mc), still_running);
      }
      // poll
      ready_fd = -1;
      mc = curl_multi_poll(multi_handle, &poll_timer_fd, 1, 10000, &ready_fd);
      // printf("polling\n");
      if (mc)
      {
        syslog(LOG_ERR, "curl_multi_poll failed:%s",
               curl_multi_strerror(mc));
        break;
      }
      /*check one-shot timer */
      struct itimerspec cur_it;
      if (timerfd_gettime(fd, &cur_it) != 0)
      {
        perror("timer_gettime failed:");
        return 1;
      }
      if (cur_it.it_value.tv_sec == 0 &&
          cur_it.it_value.tv_nsec == 0)
      {
        syslog(LOG_INFO, "time out 1 sec for mult request");
        break;
      }

      mc = curl_multi_perform(multi_handle, &still_running);
      if (mc)
      {
        syslog(LOG_ERR, "curl_multi_perform failed:%s",
               curl_multi_strerror(mc));
        /*first remove all easy handles, then re add them all*/
        break;
      }
      CURLMsg *msg;
      do
      {
        /* we check no matter ready_fd */
        msg = NULL;
        msg = curl_multi_info_read(multi_handle, &msgs_left);
        if (msg)
        {
          CURL *e = msg->easy_handle;
          struct curl_fetch_st st_fetch;
          struct curl_fetch_st *p_st_fetch;
          CURLcode c = curl_easy_getinfo(e,
                                         CURLINFO_PRIVATE, (char **)&p_st_fetch);
          if (c != CURLE_OK)
          {
            syslog(LOG_ERR, "curl_easy_getinfo CURLINFO_PRIVATE: %s",
                   curl_easy_strerror(c));
          }
          if (msg->msg == CURLMSG_DONE)
          {
            if (msg->data.result == CURLE_OK)
            {
              syslog(LOG_INFO, "[camera %d]return image size is %ld",
                     p_st_fetch->id, p_st_fetch->size);
            }
            else
            {
              syslog(LOG_ERR, "[camera %d]error %s ",
                     p_st_fetch->id, curl_easy_strerror(msg->data.result));
            }
            p_st_fetch->size = 0;
            curl_multi_remove_handle(multi_handle, msg->easy_handle);
          }
          else
          {
            syslog(LOG_ERR, "[camera %d]!CURLMSG_DONE error %s ",
                   p_st_fetch->id, curl_easy_strerror(msg->data.result));
            p_st_fetch->size = 0;
            curl_multi_remove_handle(multi_handle, msg->easy_handle);
          }
        }
        else
        {
          // no need to worry about msg==NULL
          syslog(LOG_DEBUG, "curl_multi_info_read rtn msg is %p,read end!"
                            " ready_fd %d msg left is %d",
                 msg, ready_fd, msgs_left);
        }
      } while (msg);
    } while (1);
    syslog(LOG_DEBUG, "[curl_multi_perform]still_running is %d", still_running);
  }
  /* init curl handle */

  return 0;
}
