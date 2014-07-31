// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2012 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "MDS.h"

#include "MDSContext.h"

#include "common/dout.h"
#define dout_subsys ceph_subsys_mds
//#undef dout_prefix
//#define dout_prefix "MDSContext "

void MDSInternalContext::complete(int r) {
  assert(mds != NULL);
  dout(1) << "complete: " << mds << dendl;
  dout(1) << "          " << mds->mds_lock.is_locked() << dendl;
  dout(1) << "          " << mds->mds_lock.is_locked_by_me() << dendl;
  assert(mds->mds_lock.is_locked_by_me());
  MDSContext::complete(r);
}


void MDSIOContext::complete(int r) {
  assert(mds != NULL);
  Mutex::Locker l(mds->mds_lock);
  dout(1) << "IOcomplete: " << mds << dendl;
  dout(1) << "          " << mds->mds_lock.is_locked() << dendl;
  dout(1) << "          " << mds->mds_lock.is_locked_by_me() << dendl;
  MDSContext::complete(r);
}

