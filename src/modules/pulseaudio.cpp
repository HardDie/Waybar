#include "modules/pulseaudio.hpp"

namespace waybar::modules {

Pulseaudio::Pulseaudio(const std::string &id, const Json::Value &config)
    : ALabel(config, "pulseaudio", id, "{volume}%"),
      mainloop_(nullptr),
      mainloop_api_(nullptr),
      context_(nullptr),
      sink_idx_(0),
      volume_(0),
      muted_(false),
      source_idx_(0),
      source_volume_(0),
      source_muted_(false) {
  args_.push_back(Arg{"volume", std::bind(&Pulseaudio::getVolume, this), DEFAULT | STATE});
  args_.push_back(Arg{"format_source", std::bind(&Pulseaudio::getSourceFormat, this)});
  args_.push_back(Arg{"", std::bind(&Pulseaudio::getPortIcon, this), STATE_ALT});
  args_.push_back(Arg{"desc", std::bind(&Pulseaudio::getDesc, this), TOOLTIP});
  mainloop_ = pa_threaded_mainloop_new();
  if (mainloop_ == nullptr) {
    throw std::runtime_error("pa_mainloop_new() failed.");
  }
  pa_threaded_mainloop_lock(mainloop_);
  mainloop_api_ = pa_threaded_mainloop_get_api(mainloop_);
  context_ = pa_context_new(mainloop_api_, "waybar");
  if (context_ == nullptr) {
    throw std::runtime_error("pa_context_new() failed.");
  }
  if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
    auto err =
        fmt::format("pa_context_connect() failed: {}", pa_strerror(pa_context_errno(context_)));
    throw std::runtime_error(err);
  }
  pa_context_set_state_callback(context_, contextStateCb, this);
  if (pa_threaded_mainloop_start(mainloop_) < 0) {
    throw std::runtime_error("pa_mainloop_run() failed.");
  }
  pa_threaded_mainloop_unlock(mainloop_);
  event_box_.add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
  event_box_.signal_scroll_event().connect(sigc::mem_fun(*this, &Pulseaudio::handleScroll));
}

Pulseaudio::~Pulseaudio() {
  mainloop_api_->quit(mainloop_api_, 0);
  pa_threaded_mainloop_stop(mainloop_);
  pa_threaded_mainloop_free(mainloop_);
}

void Pulseaudio::contextStateCb(pa_context *c, void *data) {
  auto pa = static_cast<Pulseaudio *>(data);
  switch (pa_context_get_state(c)) {
    case PA_CONTEXT_TERMINATED:
      pa->mainloop_api_->quit(pa->mainloop_api_, 0);
      break;
    case PA_CONTEXT_READY:
      pa_context_get_server_info(c, serverInfoCb, data);
      pa_context_set_subscribe_callback(c, subscribeCb, data);
      pa_context_subscribe(
          c,
          static_cast<enum pa_subscription_mask>(static_cast<int>(PA_SUBSCRIPTION_MASK_SINK) |
                                                 static_cast<int>(PA_SUBSCRIPTION_MASK_SOURCE)),
          nullptr,
          nullptr);
      break;
    case PA_CONTEXT_FAILED:
      pa->mainloop_api_->quit(pa->mainloop_api_, 1);
      break;
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
    default:
      break;
  }
}

bool Pulseaudio::handleScroll(GdkEventScroll *e) {
  // change the pulse volume only when no user provided
  // events are configured
  if (config_["on-scroll-up"].isString() || config_["on-scroll-down"].isString()) {
    return AModule::handleScroll(e);
  }
  auto dir = AModule::getScrollDir(e);
  if (dir == SCROLL_DIR::NONE) {
    return true;
  }
  double      volume_tick = static_cast<double>(PA_VOLUME_NORM) / 100;
  pa_volume_t change = volume_tick;
  pa_cvolume  pa_volume = pa_volume_;
  // isDouble returns true for integers as well, just in case
  if (config_["scroll-step"].isDouble()) {
    change = round(config_["scroll-step"].asDouble() * volume_tick);
  }
  if (dir == SCROLL_DIR::UP) {
    if (volume_ + 1 < 100) {
      pa_cvolume_inc(&pa_volume, change);
    }
  } else if (dir == SCROLL_DIR::DOWN) {
    if (volume_ - 1 >= 0) {
      pa_cvolume_dec(&pa_volume, change);
    }
  }
  pa_context_set_sink_volume_by_index(context_, sink_idx_, &pa_volume, volumeModifyCb, this);
  return true;
}

/*
 * Called when an event we subscribed to occurs.
 */
void Pulseaudio::subscribeCb(pa_context *context, pa_subscription_event_type_t type, uint32_t idx,
                             void *data) {
  unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
  unsigned operation = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
  if (operation != PA_SUBSCRIPTION_EVENT_CHANGE) {
    return;
  }
  if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
    pa_context_get_sink_info_by_index(context, idx, sinkInfoCb, data);
  } else if (facility == PA_SUBSCRIPTION_EVENT_SOURCE) {
    pa_context_get_source_info_by_index(context, idx, sourceInfoCb, data);
  }
}

/*
 * Called in response to a volume change request
 */
void Pulseaudio::volumeModifyCb(pa_context *c, int success, void *data) {
  auto pa = static_cast<Pulseaudio *>(data);
  if (success != 0) {
    pa_context_get_sink_info_by_index(pa->context_, pa->sink_idx_, sinkInfoCb, data);
  }
}

/*
 * Called when the requested source information is ready.
 */
void Pulseaudio::sourceInfoCb(pa_context * /*context*/, const pa_source_info *i, int /*eol*/,
                              void *data) {
  if (i != nullptr) {
    auto self = static_cast<Pulseaudio *>(data);
    auto source_volume = static_cast<float>(pa_cvolume_avg(&(i->volume))) / float{PA_VOLUME_NORM};
    self->source_volume_ = std::round(source_volume * 100.0F);
    self->source_idx_ = i->index;
    self->source_muted_ = i->mute != 0;
    self->source_desc_ = i->description;
    self->source_port_name_ = i->active_port != nullptr ? i->active_port->name : "Unknown";
    self->dp.emit();
  }
}

/*
 * Called when the requested sink information is ready.
 */
void Pulseaudio::sinkInfoCb(pa_context * /*context*/, const pa_sink_info *i, int /*eol*/,
                            void *data) {
  if (i != nullptr) {
    auto pa = static_cast<Pulseaudio *>(data);
    pa->pa_volume_ = i->volume;
    float volume = static_cast<float>(pa_cvolume_avg(&(pa->pa_volume_))) / float{PA_VOLUME_NORM};
    pa->sink_idx_ = i->index;
    pa->volume_ = std::round(volume * 100.0F);
    pa->muted_ = i->mute != 0;
    pa->desc_ = i->description;
    pa->monitor_ = i->monitor_source_name;
    pa->port_name_ = i->active_port != nullptr ? i->active_port->name : "Unknown";
    pa->dp.emit();
  }
}

/*
 * Called when the requested information on the server is ready. This is
 * used to find the default PulseAudio sink.
 */
void Pulseaudio::serverInfoCb(pa_context *context, const pa_server_info *i, void *data) {
  pa_context_get_sink_info_by_name(context, i->default_sink_name, sinkInfoCb, data);
  pa_context_get_source_info_by_name(context, i->default_source_name, sourceInfoCb, data);
}

static const std::array<std::string, 9> ports = {
    "headphones",
    "speaker",
    "hdmi",
    "headset",
    "handsfree",
    "portable",
    "car",
    "hifi",
    "phone",
};

const std::string &Pulseaudio::getPortIcon() const {
  std::string nameLC = port_name_;
  std::transform(nameLC.begin(), nameLC.end(), nameLC.begin(), ::tolower);
  for (auto const &port : ports) {
    if (nameLC.find(port) != std::string::npos) {
      return port;
    }
  }
  return port_name_;
}

const std::string Pulseaudio::getFormat() const {
  if (muted_ && config_["format-muted"].isString()) {
    return config_["format-muted"].asString();
  }
  if (config_["format-bluetooth"].isString() && monitor_.find("a2dp_sink") != std::string::npos) {
    return config_["format-bluetooth"].asString();
  }
  return ALabel::getFormat();
}

const std::vector<std::string> Pulseaudio::getClasses() const {
  std::vector<std::string> classes;
  if (muted_) {
    classes.push_back("muted");
  }
  if (monitor_.find("a2dp_sink") != std::string::npos) {
    classes.push_back("bluetooth");
  }
  return classes;
}

// TODO: find a better way to split source/sink
const std::string Pulseaudio::getSourceFormat() const {
  if (source_muted_ && config_["format-source-muted"].isString()) {
    return fmt::format(config_["format-source-muted"].asString(),
                       fmt::arg("volume", source_volume_));
  }
  if (!source_muted_ && config_["format-source"].isString()) {
    return fmt::format(config_["format-source"].asString(), fmt::arg("volume", source_volume_));
  }
  return fmt::format("{volume}%", fmt::arg("volume", source_volume_));
}

uint16_t Pulseaudio::getVolume() const { return volume_; }

const std::string &Pulseaudio::getDesc() const { return desc_; }

}  // namespace waybar::modules
