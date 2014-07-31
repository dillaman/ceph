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


#ifndef MDS_CONTEXT_H
#define MDS_CONTEXT_H

#include "include/Context.h"

class MDS;


/**
 * Completion which carries a reference to the global MDS instance
 */
class MDSContext : public Context
{
    protected:
    MDS *mds;

    public:
    MDSContext(MDS *mds_) : mds(mds_) {}
};


/**
 * Completion for an MDS-internal wait, asserts that
 * the big MDS lock is already held before calling
 * finish function.
 */
class MDSInternalContext : public MDSContext
{
    public:
    void complete(int r);
    MDSInternalContext(MDS *mds_) : MDSContext(mds_) {}

    // You're allowed to instantiate without arguments, but you may never
    // call complete() if you do.  This only exists for use with C_GatherBuilder.  Could
    // avoid this by explicitly subclassing an MDSGatherBuilder that knew to pass MDS to
    // constructor.
    MDSInternalContext() : MDSContext(NULL) {}
};


/**
 * Completion for an I/O operation, takes big MDS lock
 * before executing finish function.
 */
class MDSIOContext : public MDSContext
{
    public:
    void complete(int r);
    MDSIOContext(MDS *mds_) : MDSContext(mds_) {}
};


/**
 * No-op for callers expecting MDSInternalContext
 */
class C_MDSInternalNoop : public MDSInternalContext
{
    public:
    C_MDSInternalNoop() : MDSInternalContext(NULL) {}
    void finish(int r) {}
    void complete(int r) {}
};


/**
 * XXX FIXME this class should not exist, it is used in places
 * where a function has to handle both MDSIOContext and MDSInternalContext
 * completions, to turn the latter into the former
 */
class C_IO_Wrapper : public MDSIOContext
{
  private:
    Context *wrapped;
  public:
    C_IO_Wrapper(MDS *mds_, Context *wrapped_) : MDSIOContext(mds_), wrapped(wrapped_) {}
    virtual void finish(int r) {
      wrapped->complete(r);
    }
};

#endif  // MDS_CONTEXT_H

