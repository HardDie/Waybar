#pragma once

#include <pulse/pulseaudio.h>
#include <json/json.h>
#include <gtkmm.h>
#include <fmt/format.h>
#include <thread>
#include "util/chrono.hpp"
#include "IModule.hpp"

namespace waybar::modules {

  class Pulseaudio : public IModule {
    public:
      Pulseaudio(Json::Value config);
      auto update() -> void;
      operator Gtk::Widget &();
    private:
      static void _subscribeCb(pa_context *context,
        pa_subscription_event_type_t type, uint32_t idx, void *data);
      static void _contextStateCb(pa_context *c, void *data);
      static void _sinkInfoCb(pa_context *context, const pa_sink_info *i,
        int eol, void *data);
      static void _serverInfoCb(pa_context *context, const pa_server_info *i,
        void *data);
      Gtk::Label _label;
      std::thread _thread;
      Json::Value _config;
      pa_mainloop *_mainloop;
      pa_mainloop_api *_mainloop_api;
      pa_context *_context;
      int _volume;
      bool _muted;
  };

}