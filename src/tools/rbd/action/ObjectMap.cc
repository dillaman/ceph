// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "common/errno.h"
#include <iostream>
#include <boost/program_options.hpp>

namespace rbd {
namespace action {
namespace object_map {

namespace at = argument_types;
namespace po = boost::program_options;

static int do_object_map_rebuild(librbd::Image &image, bool no_progress)
{
  utils::ProgressContext pc("Object Map Rebuild", no_progress);
  int r = image.rebuild_object_map(pc);
  if (r < 0) {
    pc.fail();
    return r;
  }
  pc.finish();
  return 0;
}

void get_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_image_or_snap_spec_options(positional, options,
                                     at::ARGUMENT_MODIFIER_NONE);
  at::add_no_progress_option(options);
}

int execute(const po::variables_map &vm) {
  std::string pool_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, utils::get_positional_argument(vm, 0),
    &pool_name, &image_name, &snap_name, utils::SNAPSHOT_PRESENCE_PERMITTED);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, image_name, snap_name, false,
                                 &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  r = do_object_map_rebuild(image, vm[at::NO_PROGRESS].as<bool>());
  if (r < 0) {
    std::cerr << "rbd: rebuilding object map failed: " << cpp_strerror(r)
              << std::endl;
    return r;
  }
  return 0;
}

Shell::Action action(
  {"object-map", "rebuild"}, {}, "Rebuild an invalid object map.", "",
  &get_arguments, &execute);

} // namespace object_map
} // namespace action
} // namespace rbd
