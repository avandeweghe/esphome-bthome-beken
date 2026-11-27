#include "bthome_beken.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include <cmath>

// Beken SDK BLE 5.x entry points. LibreTiny pins framework-beken-bdk at a
// version whose BLE code lives in beken378/driver/ble_5_x_rw/ (API header
// beken378/driver/include/ble_api_5_x.h). Declared here directly so we don't
// depend on the SDK's internal include paths.
extern "C" {
typedef void (*bk_ble_notice_cb_t)(int notice, void *param);
typedef void (*bk_ble_cmd_cb_t)(int cmd, void *param);
void ble_init(void);   // registers the BLE interrupt handlers (LibreTiny's boot skips it)
void ble_entry(void);  // starts the BLE thread
void ble_send_msg(uint32_t data);
void ble_appm_send_gapm_reset_cmd(void);  // (re)start the stack's reset sequence
void ble_set_notice_cb(bk_ble_notice_cb_t func);
uint8_t app_ble_get_idle_actv_idx_handle(void);
int bk_ble_create_advertising(uint8_t actv_idx, unsigned char chnl_map, uint32_t intv_min, uint32_t intv_max,
                              bk_ble_cmd_cb_t callback);
int bk_ble_set_adv_data(uint8_t actv_idx, unsigned char *adv_buff, unsigned char adv_len, bk_ble_cmd_cb_t callback);
int bk_ble_start_advertising(uint8_t actv_idx, uint16_t duration, bk_ble_cmd_cb_t callback);
}

namespace esphome {
namespace bthome_beken {

static const char *const TAG = "bthome_beken";

// ble_notice_t / ble_cmd_t values in the pinned SDK (no BLE_CMD_NONE, so cmds start at 0)
static const int BLE_5_STACK_OK = 0;
static const int BLE_CREATE_ADV = 0;
static const int BLE_SET_ADV_DATA = 1;
static const int BLE_START_ADV = 3;
static const uint32_t BLE_MSG_POLL = 0;

static volatile bool g_stack_ok = false;
static volatile int g_cmd_done = -1;
static volatile int g_cmd_status = -1;
static volatile int g_cmd_status1 = -1;  // byte 1 (status if the SDK uses packed enums)

static void notice_cb(int notice, void *param) {
  if (notice == BLE_5_STACK_OK)
    g_stack_ok = true;
}

static void cmd_cb(int cmd, void *param) {
  // ble_cmd_param_t { uint8_t cmd_idx; ble_err_t status; } - 4-byte enum, status at byte 4
  const uint8_t *p = static_cast<const uint8_t *>(param);
  g_cmd_status = p ? p[4] : -2;
  g_cmd_status1 = p ? p[1] : -2;
  g_cmd_done = cmd;
}

enum State : uint8_t { WAIT_STACK, CREATE, WAIT_CREATE, SET_DATA, WAIT_DATA, START, WAIT_START, RUNNING, FAILED };
static State g_state = WAIT_STACK;
static uint8_t g_actv = 0xFF;

bool BTHomeBeken::is_broadcasting() const { return g_state == RUNNING; }

void BTHomeBeken::build_packet_() {
  uint8_t *a = this->adv_;
  uint8_t i = 0;
  a[i++] = 0x02; a[i++] = 0x01; a[i++] = 0x06;  // flags: LE general discoverable, no BR/EDR
  uint8_t len_pos = i++;
  a[i++] = 0x16;                                 // service data, 16-bit UUID
  a[i++] = 0xD2; a[i++] = 0xFC;                  // 0xFCD2 = BTHome
  a[i++] = 0x44;                                 // BTHome v2, not encrypted, trigger-based (sleepy: irregular sends)
  a[i++] = 0x00; a[i++] = this->packet_id_;      // packet id
  if (this->sent_battery_ >= 0) {
    a[i++] = 0x01; a[i++] = (uint8_t) this->sent_battery_;  // battery %
  }
  if (this->sent_moisture_ >= 0) {
    a[i++] = 0x20; a[i++] = (uint8_t) this->sent_moisture_;  // moisture
  }
  if (this->sent_tamper_ >= 0) {
    a[i++] = 0x2B; a[i++] = (uint8_t) this->sent_tamper_;  // tamper
  }
  if (this->sent_vibration_ >= 0) {
    a[i++] = 0x2C; a[i++] = (uint8_t) this->sent_vibration_;  // vibration
  }
  // Objects go in ascending id order (BTHome v2): button 0x3A before count 0x3D.
  if (this->sent_button_) {
    a[i++] = 0x3A; a[i++] = this->sent_button_;  // button event
  }
  if (this->broadcast_ms_ > 0) {
    uint16_t ms = this->broadcast_ms_ > 0xFFFF ? 0xFFFF : this->broadcast_ms_;
    a[i++] = 0x3D; a[i++] = ms & 0xFF; a[i++] = ms >> 8;  // count (uint16): wake-to-broadcast ms
  }
  a[len_pos] = i - len_pos - 1;
  uint8_t n = this->local_name_.size();
  if (i + 2 + n <= sizeof(this->adv_)) {
    a[i++] = n + 1;
    a[i++] = 0x09;  // complete local name
    for (uint8_t k = 0; k < n; k++) a[i++] = this->local_name_[k];
  }
  this->adv_len_ = i;
}

void BTHomeBeken::setup() {
  // Nothing here: starting the BLE stack inside setup() (before the main loop
  // runs) crashes the chip when Wi-Fi is off. auto_start starts it from loop().
  // Continue the packet id from the previous wake (stored in flash): the
  // counter would otherwise reset every deep sleep, and BTHome drops a packet
  // whose id matches the last one it accepted. (random_uint32() is no good here:
  // it returns the same value on every boot.)
  this->pref_ = global_preferences->make_preference<uint8_t>(0x8740BEEFUL);
  uint8_t last = 0;
  if (this->pref_.load(&last))
    this->packet_id_ = last + 1;
}

void BTHomeBeken::start() {
  if (this->started_)
    return;
  this->started_ = true;
  this->started_ms_ = millis();
  ESP_LOGI(TAG, "BLE stack starting");
  ble_set_notice_cb(notice_cb);
  ble_init();
  ble_entry();
  ble_send_msg(BLE_MSG_POLL);
  ESP_LOGI(TAG, "BLE thread started");
}

void BTHomeBeken::loop() {
  if (!this->started_) {
    if (this->auto_start_)
      this->start();
    return;
  }
  uint32_t now = millis();

  // Under LibreTiny the BLE interrupts don't drive the stack's scheduler, so poll it.
  static uint32_t last_poke = 0;
  if (now - last_poke >= 20) {
    last_poke = now;
    ble_send_msg(BLE_MSG_POLL);
  }

  // Status line every 15 s (shows up on serial and network logs).
  static uint32_t last_status = 0;
  if (now - last_status >= 15000) {
    last_status = now;
    ESP_LOGI(TAG, "Status: state %d (0 wait stack, 7 broadcasting, 8 failed), stack_ok %d, packet %u, last cmd %d status %d",
             g_state, g_stack_ok ? 1 : 0, this->packet_id_, g_cmd_done, g_cmd_status);
  }

  // Current values
  int moisture = -1;
  if (this->moisture_ != nullptr && this->moisture_->has_state())
    moisture = this->moisture_->state ? 1 : 0;
  int tamper = -1;
  if (this->tamper_ != nullptr && this->tamper_->has_state())
    tamper = this->tamper_->state ? 1 : 0;
  int vibration = -1;
  if (this->vibration_ != nullptr && this->vibration_->has_state())
    vibration = this->vibration_->state ? 1 : 0;
  int battery = -1;
  if (this->battery_ != nullptr && this->battery_->has_state() && !std::isnan(this->battery_->state))
    battery = (int) lroundf(clamp(this->battery_->state, 0.0f, 100.0f));

  switch (g_state) {
    case WAIT_STACK: {
      // The first reset is sent before the scheduler is being polled and gets
      // lost, so re-send it (every 300 ms) until the stack reports ready.
      static uint32_t last_reset = 0;
      if (g_stack_ok) {
        ESP_LOGI(TAG, "BLE stack ready after %u ms", (unsigned) (now - this->started_ms_));
        g_state = CREATE;
      } else if (now - this->started_ms_ > 100 && now - last_reset >= 300) {
        last_reset = now;
        ESP_LOGD(TAG, "Sending BLE stack reset");
        ble_appm_send_gapm_reset_cmd();
      }
      break;
    }
    case CREATE: {
      g_actv = app_ble_get_idle_actv_idx_handle();
      uint32_t units = this->adv_interval_ms_ * 8 / 5;  // 0.625 ms units
      this->step_ms_ = now;
      g_cmd_done = -1;
      if (bk_ble_create_advertising(g_actv, 7, units, units, cmd_cb) != 0) {
        ESP_LOGE(TAG, "create advertising refused");
        g_state = FAILED;
      } else {
        g_state = WAIT_CREATE;
      }
      break;
    }
    // Commands are issued one at a time, so any callback completes the pending
    // one. The command codes differ between SDKs (BK7231N: create = 0; BK7238:
    // create = 1), so don't match on them; just log what came back.
    case WAIT_CREATE:
      if (g_cmd_done >= 0) {
        ESP_LOGD(TAG, "create advertising done (cmd %d, status byte4 %d byte1 %d)", g_cmd_done, g_cmd_status, g_cmd_status1);
        g_state = SET_DATA;
      }
      break;
    case SET_DATA:
      this->sent_moisture_ = moisture;
      this->sent_tamper_ = tamper;
      this->sent_vibration_ = vibration;
      this->sent_battery_ = battery;
      this->build_packet_();
      this->step_ms_ = now;
      g_cmd_done = -1;
      if (bk_ble_set_adv_data(g_actv, this->adv_, this->adv_len_, cmd_cb) != 0) {
        ESP_LOGE(TAG, "set advertising data refused");
        g_state = FAILED;
      } else {
        g_state = WAIT_DATA;
      }
      break;
    case WAIT_DATA:
      if (g_cmd_done >= 0) {
        ESP_LOGD(TAG, "set advertising data done (cmd %d, status byte4 %d byte1 %d)", g_cmd_done, g_cmd_status, g_cmd_status1);
        g_state = START;
      }
      break;
    case START:
      this->step_ms_ = now;
      g_cmd_done = -1;
      if (bk_ble_start_advertising(g_actv, 0, cmd_cb) != 0) {
        ESP_LOGE(TAG, "start advertising refused");
        g_state = FAILED;
      } else {
        g_state = WAIT_START;
      }
      break;
    case WAIT_START:
      if (g_cmd_done >= 0) {
        ESP_LOGD(TAG, "start advertising done (cmd %d, status byte4 %d byte1 %d)", g_cmd_done, g_cmd_status, g_cmd_status1);
        this->broadcast_ms_ = now;
        this->last_send_ms_ = now;
        // Re-send right away so the packet carries the wake-to-broadcast time.
        this->packet_id_++;
        this->build_packet_();
        bk_ble_set_adv_data(g_actv, this->adv_, this->adv_len_, cmd_cb);
        this->pref_.save(&this->packet_id_);
        g_state = RUNNING;
        ESP_LOGI(TAG, "Broadcasting as '%s' %u ms after boot (moisture %d, tamper %d, vibration %d, battery %d)",
                 this->local_name_.c_str(), (unsigned) now, this->sent_moisture_, this->sent_tamper_,
                 this->sent_vibration_, this->sent_battery_);
        for (auto &cb : this->on_packet_) cb();
        for (auto &cb : this->on_broadcast_) cb();
      }
      break;
    case RUNNING: {
      // New data -> new packet id, so Home Assistant treats it as a fresh reading.
      uint8_t button = this->pending_button_;
      bool refresh = this->refresh_ms_ > 0 && now - this->last_send_ms_ >= this->refresh_ms_;
      // Keep each packet on air >= 600 ms (~12 copies) before replacing it, so
      // quick successive changes don't overwrite a packet before it's heard.
      if (now - this->last_send_ms_ < 600)
        break;
      if (refresh || moisture != this->sent_moisture_ || tamper != this->sent_tamper_ ||
          vibration != this->sent_vibration_ || battery != this->sent_battery_ || button != this->sent_button_) {
        this->last_send_ms_ = now;
        this->pending_button_ = 0;
        this->sent_moisture_ = moisture;
        this->sent_tamper_ = tamper;
        this->sent_vibration_ = vibration;
        this->sent_battery_ = battery;
        this->sent_button_ = button;
        this->packet_id_++;
        this->build_packet_();
        bk_ble_set_adv_data(g_actv, this->adv_, this->adv_len_, cmd_cb);
        this->pref_.save(&this->packet_id_);
        ESP_LOGD(TAG, "Packet %u: moisture %d, tamper %d, vibration %d, battery %d, button %u", this->packet_id_,
                 moisture, tamper, vibration, battery, button);
        for (auto &cb : this->on_packet_) cb();
      }
      break;
    }
    case FAILED:
      break;
  }

  if ((g_state == WAIT_CREATE || g_state == WAIT_DATA || g_state == WAIT_START) && now - this->step_ms_ > 5000) {
    ESP_LOGW(TAG, "BLE command timed out in state %d (last cmd %d, status %d)", g_state, g_cmd_done, g_cmd_status);
    g_state = FAILED;
  }
}

void BTHomeBeken::dump_config() {
  ESP_LOGCONFIG(TAG, "BTHome over Beken BLE:");
  ESP_LOGCONFIG(TAG, "  Local name: %s", this->local_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Advertising interval: %u ms", (unsigned) this->adv_interval_ms_);
  ESP_LOGCONFIG(TAG, "  State: %d (7 = broadcasting, 8 = failed), packet %u", g_state, this->packet_id_);
}

}  // namespace bthome_beken
}  // namespace esphome
