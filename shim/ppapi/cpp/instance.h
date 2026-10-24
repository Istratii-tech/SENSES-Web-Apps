// Minimal pp::Instance / pp::InstanceHandle for the wasm port. Part of the
// arc-wasm shim layer.
#ifndef PPAPI_CPP_INSTANCE_H_
#define PPAPI_CPP_INSTANCE_H_

#include "ppapi/c/pp_types.h"

namespace pp {

class Instance {
 public:
  explicit Instance(PP_Instance instance) : pp_instance_(instance) {}
  virtual ~Instance() {}
  PP_Instance pp_instance() const { return pp_instance_; }

 private:
  PP_Instance pp_instance_;
};

class InstanceHandle {
 public:
  InstanceHandle(Instance* instance)  // NOLINT
      : pp_instance_(instance ? instance->pp_instance() : 0) {}
  explicit InstanceHandle(PP_Instance pp_instance)
      : pp_instance_(pp_instance) {}
  PP_Instance pp_instance() const { return pp_instance_; }

 private:
  PP_Instance pp_instance_;
};

class View {};

}  // namespace pp

#endif  // PPAPI_CPP_INSTANCE_H_
