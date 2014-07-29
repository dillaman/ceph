// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_LOGMONITOR_H
#define CEPH_LOGMONITOR_H

#include <map>
#include <set>
using namespace std;

#include "include/types.h"
#include "msg/Messenger.h"
#include "PaxosService.h"

#include "common/LogEntry.h"
#include "messages/MLog.h"

class MMonCommand;

class LogMonitor : public PaxosService,
                   public md_config_obs_t {
private:
  multimap<utime_t,LogEntry> pending_log;
  LogSummary pending_summary, summary;


  struct log_channel_info {
    string channel;
    string prio;
    string file;

    string syslog_facility;
    string syslog_level;
    bool to_syslog;

    log_channel_info() : to_syslog(false) {}
  };
  map<string, log_channel_info> log_channels;

  void update_log_channels();

  void create_initial();
  void update_from_paxos(bool *need_bootstrap);
  void create_pending();  // prepare a new pending
  // propose pending update to peers
  void encode_pending(MonitorDBStore::Transaction *t);
  virtual void encode_full(MonitorDBStore::Transaction *t);
  version_t get_trim_to();
  bool preprocess_query(PaxosServiceMessage *m);  // true if processed.
  bool prepare_update(PaxosServiceMessage *m);

  bool preprocess_log(MLog *m);
  bool prepare_log(MLog *m);
  void _updated_log(MLog *m);

  bool should_propose(double& delay);

  bool should_stash_full() {
    // commit a LogSummary on every commit
    return true;
  }

  struct C_Log : public Context {
    LogMonitor *logmon;
    MLog *ack;
    C_Log(LogMonitor *p, MLog *a) : logmon(p), ack(a) {}
    void finish(int r) {
      if (r == -ECANCELED) {
	if (ack)
	  ack->put();
	return;
      }
      logmon->_updated_log(ack);
    }    
  };

  bool preprocess_command(MMonCommand *m);
  bool prepare_command(MMonCommand *m);

  bool _create_sub_summary(MLog *mlog, int level);
  void _create_sub_incremental(MLog *mlog, int level, version_t sv);

  void store_do_append(MonitorDBStore::Transaction *t,
		       const string& key, bufferlist& bl);

 public:
  LogMonitor(Monitor *mn, Paxos *p, const string& service_name) 
    : PaxosService(mn, p, service_name) { }
  
  void tick();  // check state, take actions

  void check_subs();
  void check_sub(Subscription *s);

  /**
   * translate log sub name ('log-info') to integer id
   *
   * @param n name
   * @return id, or -1 if unrecognized
   */
  int sub_name_to_id(const string& n);

  void init() {
    g_conf->add_observer(this);
  }

  void on_shutdown() {
    g_conf->remove_observer(this);
  }

  const chat **get_tracket_conf_keys() const {
    static const char* KEYS[] = {
      "mon_cluster_log_to_syslog",
      "mon_cluster_log_to_syslog_level",
      "mon_cluster_log_to_syslog_facility",
      "mon_cluster_log_file",
      "mon_cluster_log_file_level",
      NULL
    };
    return KEYS;
  }
  void handle_conf_change(const struct md_config_t *conf,
                          const std::set<std::string> &changed);
};
#endif
