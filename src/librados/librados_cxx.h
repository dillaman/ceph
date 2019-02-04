// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef LIBRADOS_CXX_H
#define LIBRADOS_CXX_H

/// bump ever major release
#define LIBRADOS_CXX_VERSION "14.2.0"

#define LIBRADOS_CXX_API(prefix, fn)                      \
  asm(".symver "                                          \
      #prefix #fn ", "                                    \
      #prefix #fn "@@LIBRADOS_" LIBRADOS_CXX_VERSION)

#endif // LIBRADOS_CXX_H
