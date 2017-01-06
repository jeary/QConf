#include "zookeeper.h"
#include "slash_string.h"

#include <unistd.h>
#include <pthread.h>

#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <vector>
#include <iostream>

#include "monitor_options.h"
#include "monitor_log.h"
#include "monitor_const.h"

using namespace std;

MonitorOptions::MonitorOptions(const string &conf_path)
      : base_conf(new slash::BaseConf(conf_path)),
        daemon_mode(0),
        auto_restart(0),
        monitor_host_name(""),
        log_level(2),
        conn_retry_count(3),
        scan_interval(3),
        zk_host("127.0.0.1:2181"),
        zk_log_path("logs"),
        zk_recv_timeout(3000),
        zk_log_file(NULL),
        waiting_index(MAX_THREAD_NUM),
        need_rebalance(false) {
  service_map.clear();
}

MonitorOptions::~MonitorOptions() {
  if (zk_log_file) {
    // Set the zookeeper log stream to be default stderr
    zoo_set_log_stream(NULL);
    LOG(LOG_DEBUG, "zkLog close ...");
    fclose(zk_log_file);
    zk_log_file = NULL;
  }
  delete base_conf;
}

int MonitorOptions::Load() {
  if (base_conf->LoadConf() != 0)
    return kOtherError;

  Log::init(MAX_LOG_LEVEL);

  base_conf->GetConfBool(DAEMON_MODE, &daemon_mode);
  base_conf->GetConfBool(AUTO_RESTART, &auto_restart);
  base_conf->GetConfInt(LOG_LEVEL, &log_level);
  log_level = max(log_level, MIN_LOG_LEVEL);
  log_level = min(log_level, MAX_LOG_LEVEL);
  // Find the zk host this monitor should focus on. Their idc should be the same
  char hostname[128] = {0};
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    LOG(LOG_ERROR, "get host name failed");
    return kOtherError;
  }
  monitor_host_name.assign(hostname);

  vector<string> word;
  slash::StringSplit(monitor_host_name, '.', word);
  bool find_zk_host = false;
  for (auto iter = word.begin(); iter != word.end(); iter++) {
    if (base_conf->GetConfStr(ZK_HOST + *iter, &zk_host)) {
      find_zk_host = true;
      break;
    }
  }
  if (!find_zk_host) {
    LOG(LOG_ERROR, "get zk host name failed");
    return kOtherError;
  }

  base_conf->GetConfInt(CONN_RETRY_COUNT, &conn_retry_count);
  base_conf->GetConfInt(SCAN_INTERVAL, &scan_interval);
  base_conf->GetConfStr(INSTANCE_NAME, &instance_name);
  instance_name = instance_name.empty() ? DEFAULT_INSTANCE_NAME : instance_name;
  base_conf->GetConfStr(ZK_LOG_PATH, &zk_log_path);
  base_conf->GetConfInt(ZK_RECV_TIMEOUT, &zk_recv_timeout);

  // Reload the config result in the change of loglevel in Log
  Log::init(log_level);

  // Set zookeeper log path
  int ret;
  if ((ret = SetZkLog()) != kSuccess) {
    LOG(LOG_ERROR, "set zk log path failed");
    return ret;
  }
  return kSuccess;
}

int MonitorOptions::SetZkLog() {
  if (zk_log_path.size() <= 0) {
    return kZkFailed;
  }
  zk_log_file = fopen(zk_log_path.c_str(), "a+");
  if (!zk_log_file) {
    LOG(LOG_ERROR, "log file open failed. path:%s. error:%s",
        zk_log_path.c_str(), strerror(errno));
    return kOpenFileFailed;
  }
  //set the log file stream of zookeeper
  zoo_set_log_stream(zk_log_file);
  zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);
  LOG(LOG_INFO, "zoo_set_log_stream path:%s", zk_log_path.c_str());

  return kSuccess;
}

void MonitorOptions::Debug() {
  LOG(LOG_INFO, "daemonMode: %d", daemon_mode);
  LOG(LOG_INFO, "autoRestart: %d", auto_restart);
  LOG(LOG_INFO, "logLevel: %d", log_level);
  LOG(LOG_INFO, "connRetryCount: %d", conn_retry_count);
  LOG(LOG_INFO, "scanInterval: %d", scan_interval);
  LOG(LOG_INFO, "instanceName: %s", instance_name.c_str());
  LOG(LOG_INFO, "zkHost: %s", zk_host.c_str());
  LOG(LOG_INFO, "zkLogPath: %s", zk_log_path.c_str());
}

void MonitorOptions::DebugServiceMap() {
  for (auto it = service_map.begin(); it != service_map.end(); ++it) {
    LOG(LOG_INFO, "path: %s", (it->first).c_str());
    LOG(LOG_INFO, "host: %s", (it->second).host.c_str());
    LOG(LOG_INFO, "port: %d", (it->second).port);
    LOG(LOG_INFO, "service father: %s", (it->second).service_father.c_str());
    LOG(LOG_INFO, "status: %d", (it->second).status);
  }
}

int MonitorOptions::GetAndAddWaitingIndex() {
  int index;
  int service_father_num = service_father_to_ip.size();
  slash::MutexLock lw(&waiting_index_lock);
  index = waiting_index;
  waiting_index = (waiting_index+1) % service_father_num;
  slash::MutexLock lh(&has_thread_lock);
  while (has_thread[waiting_index]) {
    waiting_index = (waiting_index + 1) % service_father_num;
  }
  return index;
}
