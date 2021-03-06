/****************************************************************************
 *
 * Copyright 2019 Jozef Kralik All Rights Reserved.
 * Copyright 2018 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

#include "oc_api.h"
#include "oc_pki.h"
#include <signal.h>
#include <inttypes.h>

static int quit;

#if defined(_WIN32)
#include <windows.h>

static CONDITION_VARIABLE cv;
static CRITICAL_SECTION cs;

static void
signal_event_loop(void)
{
  WakeConditionVariable(&cv);
}

void
handle_signal(int signal)
{
  signal_event_loop();
  quit = 1;
}

static int
init(void)
{
  InitializeCriticalSection(&cs);
  InitializeConditionVariable(&cv);

  signal(SIGINT, handle_signal);
  return 0;
}

static void
run(void)
{
  while (quit != 1) {
    oc_clock_time_t next_event = oc_main_poll();
    if (next_event == 0) {
      SleepConditionVariableCS(&cv, &cs, INFINITE);
    } else {
      oc_clock_time_t now = oc_clock_time();
      if (now < next_event) {
        SleepConditionVariableCS(
          &cv, &cs, (DWORD)((next_event - now) * 1000 / OC_CLOCK_SECOND));
      }
    }
  }
}

#elif defined(__linux__) || defined(__ANDROID_API__)
#include <pthread.h>

static pthread_mutex_t mutex;
static pthread_cond_t cv;

static void
signal_event_loop(void)
{
  pthread_mutex_lock(&mutex);
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mutex);
}

void
handle_signal(int signal)
{
  if (signal == SIGPIPE) {
    return;
  }
  signal_event_loop();
  quit = 1;
}

static int
init(void)
{
  struct sigaction sa;
  sigfillset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = handle_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGPIPE, &sa, NULL);

  if (pthread_mutex_init(&mutex, NULL) < 0) {
    OC_ERR("pthread_mutex_init failed!");
    return -1;
  }
  return 0;
}

static void
run(void)
{
  while (quit != 1) {
    oc_clock_time_t next_event = oc_main_poll();
    pthread_mutex_lock(&mutex);
    if (next_event == 0) {
      pthread_cond_wait(&cv, &mutex);
    } else {
      struct timespec ts;
      ts.tv_sec = (next_event / OC_CLOCK_SECOND);
      ts.tv_nsec = (next_event % OC_CLOCK_SECOND) * 1.e09 / OC_CLOCK_SECOND;
      pthread_cond_timedwait(&cv, &mutex, &ts);
    }
    pthread_mutex_unlock(&mutex);
  }
}

#else
#error "Unsupported OS"
#endif

// define application specific values.
static const char *spec_version = "ocf.1.0.0";
static const char *data_model_version = "ocf.res.1.0.0";

static const char *resource_rt = "core.light";
static const char *device_rt = "oic.d.cloudDevice";
static const char *device_name = "Cloud Device";

static const char *manufacturer = "ocfcloud.com";
static const char *cis = "coap+tcp://127.0.0.1:5683";
static const char *auth_code = "test";
static const char *sid = "00000000-0000-0000-0000-000000000001";
static const char *apn = "test";
oc_resource_t *res1;
oc_resource_t *res2;

static void
cloud_status_handler(oc_cloud_context_t *ctx, oc_cloud_status_t status,
                     void *data)
{
  (void)data;
  PRINT("\nCloud Manager Status:\n");
  if (status & OC_CLOUD_REGISTERED) {
    PRINT("\t\t-Registered\n");
  }
  if (status & OC_CLOUD_TOKEN_EXPIRY) {
    PRINT("\t\t-Token Expiry: ");
    if (ctx) {
      PRINT("%d\n", oc_cloud_get_token_expiry(ctx));
    } else {
      PRINT("\n");
    }
  }
  if (status & OC_CLOUD_FAILURE) {
    PRINT("\t\t-Failure\n");
  }
  if (status & OC_CLOUD_LOGGED_IN) {
    PRINT("\t\t-Logged In\n");
  }
  if (status & OC_CLOUD_LOGGED_OUT) {
    PRINT("\t\t-Logged Out\n");
  }
  if (status & OC_CLOUD_DEREGISTERED) {
    PRINT("\t\t-DeRegistered\n");
  }
  if (status & OC_CLOUD_REFRESHED_TOKEN) {
    PRINT("\t\t-Refreshed Token\n");
  }
}

static int
app_init(void)
{
  int ret = oc_init_platform(manufacturer, NULL, NULL);
  ret |= oc_add_device("/oic/d", device_rt, device_name, spec_version,
                       data_model_version, NULL, NULL);
  return ret;
}

struct light_t
{
  bool state;
  int64_t power;
};

struct light_t light1 = { 0 };
struct light_t light2 = { 0 };

static void
get_handler(oc_request_t *request, oc_interface_mask_t iface, void *user_data)
{
  struct light_t *light = (struct light_t *)user_data;

  PRINT("get_handler:\n");

  oc_rep_start_root_object();
  switch (iface) {
  case OC_IF_BASELINE:
    oc_process_baseline_interface(request->resource);
  /* fall through */
  case OC_IF_RW:
    oc_rep_set_boolean(root, state, light->state);
    oc_rep_set_int(root, power, light->power);
    oc_rep_set_text_string(root, name, "Light");
    break;
  default:
    break;
  }
  oc_rep_end_root_object();
  oc_send_response(request, OC_STATUS_OK);
}

static void
post_handler(oc_request_t *request, oc_interface_mask_t iface_mask,
             void *user_data)
{
  struct light_t *light = (struct light_t *)user_data;
  (void)iface_mask;
  printf("post_handler:\n");
  oc_rep_t *rep = request->request_payload;
  while (rep != NULL) {
    char *key = oc_string(rep->name);
    printf("key: %s ", key);
    if (key && !strcmp(key, "state")) {
      switch (rep->type) {
      case OC_REP_BOOL:
        light->state = rep->value.boolean;
        PRINT("value: %d\n", light->state);
        break;
      default:
        oc_send_response(request, OC_STATUS_BAD_REQUEST);
        return;
      }
    } else if (key && !strcmp(key, "power")) {
      switch (rep->type) {
      case OC_REP_INT:
        light->power = rep->value.integer;
        PRINT("value: %" PRId64 "\n", light->power);
        break;
      default:
        oc_send_response(request, OC_STATUS_BAD_REQUEST);
        return;
      }
    }
    rep = rep->next;
  }
  oc_send_response(request, OC_STATUS_CHANGED);
}

static void
register_resources(void)
{
  res1 = oc_new_resource(NULL, "/light/1", 1, 0);
  oc_resource_bind_resource_type(res1, resource_rt);
  oc_resource_bind_resource_interface(res1, OC_IF_RW);
  oc_resource_set_default_interface(res1, OC_IF_RW);
  oc_resource_set_discoverable(res1, true);
  oc_resource_set_observable(res1, true);
  oc_resource_set_request_handler(res1, OC_GET, get_handler, &light1);
  oc_resource_set_request_handler(res1, OC_POST, post_handler, &light1);
  oc_cloud_add_resource(res1);
  oc_add_resource(res1);

  res2 = oc_new_resource(NULL, "/light/2", 1, 0);
  oc_resource_bind_resource_type(res2, resource_rt);
  oc_resource_bind_resource_interface(res2, OC_IF_RW);
  oc_resource_set_default_interface(res2, OC_IF_RW);
  oc_resource_set_discoverable(res2, true);
  oc_resource_set_observable(res2, true);
  oc_resource_set_request_handler(res2, OC_GET, get_handler, &light2);
  oc_resource_set_request_handler(res2, OC_POST, post_handler, &light2);
  oc_cloud_add_resource(res2);
  oc_add_resource(res2);
}

void
factory_presets_cb(size_t device, void *data)
{
  (void)device;
  (void)data;
#if defined(OC_SECURITY) && defined(OC_PKI)
  // This installs the root CA certificate for the
  // https://portal.try.plgd.cloud/ OCF Cloud
  const char *cloud_ca =
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIIBhDCCASmgAwIBAgIQdAMxveYP9Nb48xe9kRm3ajAKBggqhkjOPQQDAjAxMS8w\r\n"
    "LQYDVQQDEyZPQ0YgQ2xvdWQgUHJpdmF0ZSBDZXJ0aWZpY2F0ZXMgUm9vdCBDQTAe\r\n"
    "Fw0xOTExMDYxMjAzNTJaFw0yOTExMDMxMjAzNTJaMDExLzAtBgNVBAMTJk9DRiBD\r\n"
    "bG91ZCBQcml2YXRlIENlcnRpZmljYXRlcyBSb290IENBMFkwEwYHKoZIzj0CAQYI\r\n"
    "KoZIzj0DAQcDQgAEaNJi86t5QlZiLcJ7uRMNlcwIpmFiJf9MOqyz2GGnGVBypU6H\r\n"
    "lwZHY2/l5juO/O4EH2s9h3HfcR+nUG2/tFzFEaMjMCEwDgYDVR0PAQH/BAQDAgEG\r\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSQAwRgIhAM7gFe39UJPIjIDE\r\n"
    "KrtyPSIGAk0OAO8txhow1BAGV486AiEAqszg1fTfOHdE/pfs8/9ZP5gEVVkexRHZ\r\n"
    "JCYVaa2Spbg=\r\n"
    "-----END CERTIFICATE-----\r\n";
  int rootca_credid = oc_pki_add_trust_anchor(
    0, (const unsigned char *)cloud_ca, strlen(cloud_ca));
  if (rootca_credid < 0) {
    PRINT("ERROR installing root cert\n");
    return;
  }
#endif /* OC_SECURITY && OC_PKI */
}

int
main(int argc, char *argv[])
{
  PRINT("Default parameters: device_name: %s, auth_code: %s, cis: %s, sid: %s, "
        "apn: %s\n",
        device_name, auth_code, cis, sid, apn);
  if (argc == 1) {
    PRINT("./cloud_client <device-name-without-spaces> <auth-code> <cis> <sid> "
          "<apn>\n"
          "Using the default values\n");
  }
  if (argc > 1) {
    device_name = argv[1];
    PRINT("device_name: %s\n", argv[1]);
  }
  if (argc > 2) {
    auth_code = argv[2];
    PRINT("auth_code: %s\n", argv[2]);
  }
  if (argc > 3) {
    cis = argv[3];
    PRINT("cis : %s\n", argv[3]);
  }
  if (argc > 4) {
    sid = argv[4];
    PRINT("sid: %s\n", argv[4]);
  }
  if (argc > 5) {
    apn = argv[5];
    PRINT("apn: %s\n", argv[5]);
  }

  int ret = init();
  if (ret < 0) {
    return ret;
  }

  static const oc_handler_t handler = { .init = app_init,
                                        .signal_event_loop = signal_event_loop,
                                        .register_resources =
                                          register_resources };
#ifdef OC_STORAGE
  oc_storage_config("./cloud_server_creds/");
#endif /* OC_STORAGE */
  oc_set_factory_presets_cb(factory_presets_cb, NULL);

  ret = oc_main_init(&handler);
  if (ret < 0)
    return ret;

  oc_cloud_context_t *ctx = oc_cloud_get_context(0);
  if (ctx) {
    oc_cloud_manager_start(ctx, cloud_status_handler, NULL);
    oc_cloud_provision_conf_resource(ctx, cis, auth_code, sid, apn);
  }

  run();

  oc_cloud_manager_stop(ctx);
  oc_main_shutdown();
  return 0;
}
