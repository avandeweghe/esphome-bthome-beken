#pragma once

#include <string>
#include <vector>
#include <functional>
#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace bthome_beken {

class BTHomeBeken : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_local_name(const std::string &name) { this->local_name_ = name; }
  void set_adv_interval_ms(uint32_t ms) { this->adv_interval_ms_ = ms; }
  void set_moisture(binary_sensor::BinarySensor *s) { this->moisture_ = s; }
  void set_tamper(binary_sensor::BinarySensor *s) { this->tamper_ = s; }
  void set_vibration(binary_sensor::BinarySensor *s) { this->vibration_ = s; }
  void set_battery(sensor::Sensor *s) { this->battery_ = s; }
  void set_auto_start(bool v) { this->auto_start_ = v; }
  void set_refresh_interval_ms(uint32_t ms) { this->refresh_ms_ = ms; }
  // Start the BLE stack (once). Called from setup() when auto_start is true.
  void start();
  bool is_started() const { return this->started_; }
  void add_on_broadcast(std::function<void()> &&cb) { this->on_broadcast_.push_back(std::move(cb)); }
  void add_on_packet(std::function<void()> &&cb) { this->on_packet_.push_back(std::move(cb)); }

  // Queue a BTHome button event (0x01 press, 0x02 double, 0x04 long press).
  void send_button_event(uint8_t event) { this->pending_button_ = event; }
  bool is_broadcasting() const;

 protected:
  void build_packet_();

  std::string local_name_{"BTHome"};
  uint32_t adv_interval_ms_{100};
  binary_sensor::BinarySensor *moisture_{nullptr};
  binary_sensor::BinarySensor *tamper_{nullptr};
  binary_sensor::BinarySensor *vibration_{nullptr};
  sensor::Sensor *battery_{nullptr};
  std::vector<std::function<void()>> on_broadcast_;
  std::vector<std::function<void()>> on_packet_;

  bool auto_start_{true};
  uint32_t refresh_ms_{0};  // re-send with a new packet id this often (0 = only on change)
  uint32_t last_send_ms_{0};
  bool started_{false};
  uint32_t started_ms_{0};
  uint32_t step_ms_{0};
  uint32_t broadcast_ms_{0};  // millis() since boot when advertising started
  uint8_t pending_button_{0};
  uint8_t sent_button_{0};
  int sent_moisture_{-1};
  int sent_tamper_{-1};
  int sent_vibration_{-1};
  int sent_battery_{-1};
  uint8_t packet_id_{0};
  ESPPreferenceObject pref_;
  uint8_t adv_[31];
  uint8_t adv_len_{0};
};

class BroadcastTrigger : public Trigger<> {
 public:
  explicit BroadcastTrigger(BTHomeBeken *parent) {
    parent->add_on_broadcast([this]() { this->trigger(); });
  }
};

class PacketTrigger : public Trigger<> {
 public:
  explicit PacketTrigger(BTHomeBeken *parent) {
    parent->add_on_packet([this]() { this->trigger(); });
  }
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<BTHomeBeken> {
 public:
  void play(const Ts &...x) override { this->parent_->start(); }
};

template<typename... Ts> class ButtonEventAction : public Action<Ts...>, public Parented<BTHomeBeken> {
 public:
  void set_event(uint8_t event) { this->event_ = event; }
  void play(const Ts &...x) override { this->parent_->send_button_event(this->event_); }

 protected:
  uint8_t event_{1};
};

}  // namespace bthome_beken
}  // namespace esphome
