---
applyTo: '**'
---

{
  "module": "wifi_connection_manager",
  "version": "1.0.0",
  "last_updated": "2025-08-31",
  "device": {
    "name": "OPDI_SKPR",
    "hostname_default": "opdi-skpr",
    "mdns_default": "opdi-skpr.local",
    "ethernet_supported": false
  },
  "architecture": {
    "stack_split": "esp_hosted",
    "host": "ESP32-P4 (app + lwIP)",
    "nwp": "ESP32-C6 (Wi-Fi/BT stack)",
    "transport_default": "spi",
    "transport_options": ["spi", "sdio", "uart"],
    "bringup_sequence": [
      "init_nvs",
      "start_esp_hosted_link",
      "wait_nwp_ready",
      "load_profiles",
      "enter_state_machine"
    ],
    "country_code": "IL"
  },
  "policies": {
    "profiles_max": 20,
    "selection_order": "MRU",
    "tie_breaker": "RSSI_if_same_recency",
    "hidden_ssid_supported": true,
    "sta_timeout_s": 60,
    "sta_total_retries_before_ap": 20,
    "ap_retry_period_s": 120,
    "scan_min_rssi_dbm": -90,
    "fallback_ap": {
      "enabled": true,
      "security": "open",
      "ssid_format": "OPDI_SKPR-XXXX",
      "ssid_suffix": "last4hex(sta_mac)",
      "channel_default": 1
    }
  },
  "state_machine": {
    "states": ["INIT", "STA_CONNECT", "STA_CONNECTED", "AP_ACTIVE", "FACTORY_RESET"],
    "transitions": [
      {"from": "INIT", "to": "STA_CONNECT", "on": "profiles_loaded_and_nwp_ready"},
      {"from": "STA_CONNECT", "to": "STA_CONNECTED", "on": "got_ip"},
      {"from": "STA_CONNECT", "to": "AP_ACTIVE", "on": "all_candidates_failed_or_retries_exceeded"},
      {"from": "STA_CONNECTED", "to": "STA_CONNECT", "on": "disconnected"},
      {"from": "AP_ACTIVE", "to": "STA_CONNECT", "on": "periodic_reconnect_timer"},
      {"from": "FACTORY_RESET", "to": "AP_ACTIVE", "on": "erase_done"}
    ],
    "events": {
      "wifi_disconnected_reasons": "propagate_raw_reason_code",
      "ip_acquired": "emit_sta_connected",
      "ap_started": "emit_ap_active"
    }
  },
  "timers": {
    "sta_per_ssid_timeout_s": 60,
    "ap_periodic_retry_s": 120,
    "scan_debounce_s": 30,
    "link_drop_grace_s": 5
  },
  "scanning": {
    "mode": "active",
    "channel_plan": "country_code_IL",
    "filter": {
      "min_rssi_dbm": -90
    },
    "intervals": {
      "on_boot": true,
      "on_link_drop": true,
      "in_ap_mode_periodic": true
    }
  },
  "ap_mode": {
    "ssid": "OPDI_SKPR-XXXX",
    "security": "open",
    "channel": 1,
    "ip": "192.168.4.1",
    "dhcp_range": "192.168.4.2-192.168.4.20",
    "mdns": "opdi-skpr.local",
    "http_available": true
  },
  "indicators": {
    "green_led": {
      "gpio": 22,
      "active_low": true,
      "patterns": {
        "sta_connected": "solid",
        "ap_active": "blink_1hz",
        "other": "off"
      }
    },
    "red_led": {
      "gpio": 21,
      "active_low": true,
      "patterns": {
        "reconnecting": "blink_2hz",
        "critical_fault": "solid",
        "tamper_open": "solid_override"
      },
      "priority_order_high_to_low": ["tamper_open", "critical_fault", "wifi_reconnecting", "event_flash"]
    },
    "tamper_override": true
  },
  "io_integration": {
    "rf_tts_trigger": {
      "gpio": 46,
      "polarity": "active_high",
      "debounce_ms": 30
    },
    "tamper": {
      "gpio": 27,
      "default_polarity": "active_low",
      "debounce_ms": 30,
      "led_override": "red_led_solid_on_while_open"
    }
  },
  "data_model": {
    "nvs_namespaces": {
      "net": {
        "keys": [
          "meta/version:u8",
          "mru:list<hash(ssid)>",
          "cred/<sha1(ssid)>:{ssid,psk?,auth,hidden,last_ts,success_count}"
        ]
      }
    },
    "persistence": {
      "ssid_keying": "sha1(ssid)",
      "psk_storage": "plaintext (Phase2: flash_encryption)",
      "factory_reset": "erase_namespace(net/*)"
    }
  },
  "kconfig": {
    "CONFIG_OPDI_HOSTED_BUS": "spi",
    "CONFIG_OPDI_COUNTRY_CODE": "IL",
    "CONFIG_OPDI_NET_MAX_PROFILES": 20,
    "CONFIG_OPDI_NET_STA_TIMEOUT_S": 60,
    "CONFIG_OPDI_NET_RETRIES": 20,
    "CONFIG_OPDI_NET_RETRY_PERIOD_S": 120,
    "CONFIG_OPDI_NET_SCAN_MIN_RSSI": -90,
    "CONFIG_OPDI_AP_CHANNEL": 1,
    "CONFIG_OPDI_AP_OPEN": true,
    "CONFIG_OPDI_DHCP_HOSTNAME": "opdi-skpr"
  },
  "public_api": {
    "rest": [
      {"method": "GET", "path": "/api/v1/net/sta/status", "desc": "Current state, RSSI (TBD), IP (TBD)"},
      {"method": "GET", "path": "/api/v1/net/sta/profiles", "desc": "List saved networks (SSID masked)"},
      {"method": "POST", "path": "/api/v1/net/sta/profiles", "body": "{ssid, psk, auth?, hidden?}", "desc": "Add/replace"},
      {"method": "DELETE", "path": "/api/v1/net/sta/profiles/{id}", "desc": "Forget by id/hash"},
      {"method": "POST", "path": "/api/v1/net/sta/connect", "body": "{ssid?}", "desc": "Trigger connect to specific SSID or MRU"},
      {"method": "GET", "path": "/api/v1/net/scan", "desc": "Active scan; returns [{ssid,bssid,rssi,auth,ch}]"},
      {"method": "POST", "path": "/api/v1/net/ap/config", "body": "{ssid?,channel?}", "desc": "Configure fallback AP"}
    ],
    "websocket_events": [
      {"type": "net", "sub": "sta_connected", "payload": "{ip,rssi,bssid}"},
      {"type": "net", "sub": "sta_disconnected", "payload": "{reason}"},
      {"type": "net", "sub": "ap_active", "payload": "{ssid,channel}"}
    ]
  },
  "error_handling": {
    "reasons_passthrough": true,
    "backoff_strategy": "fixed_period_in_ap_mode",
    "max_retries": 20,
    "edge_cases": [
      "duplicate_ssid_different_bssid",
      "hidden_ssid_direct_connect",
      "invalid_psk",
      "country_code_channel_mismatch"
    ]
  },
  "security": {
    "sta_modes": ["WPA2-PSK", "WPA3-PSK"],
    "ap_mode": "open (provisioning only)",
    "authz": "web_ui username/password required",
    "phase2": {
      "https_tls": true,
      "secure_boot": "planned",
      "flash_encryption": "planned"
    }
  },
  "observability": {
    "logs": {
      "tag": "opdi_net",
      "levels": ["I", "W", "E"],
      "fields": ["state", "ssid_hash", "rssi", "reason"]
    },
    "metrics": ["connect_attempts", "success_count", "avg_connect_time_ms", "scan_count"],
    "ws_or_mqtt_export": {
      "enabled": true,
      "topics": ["OPDI_SKPR/system/metrics"]
    }
  },
  "acceptance_criteria": {
    "boot_to_ready_s": 8,
    "sta_reconnect_behavior": "enter_ap_after_20_failed_attempts_then_retry_every_120s",
    "ap_provisioning": "SSID visible and HTTP reachable",
    "hidden_ssid": "connect_without_scan_presence",
    "indicators": "match_patterns_and_tamper_override_rules"
  },
  "unit_testing": {
    "host": [
      "mru_selection_order",
      "retry_and_backoff_to_ap",
      "hidden_ssid_direct_connect",
      "nvs_profile_roundtrip_and_migration"
    ],
    "target": [
      "ap_bootstrap_and_provision",
      "sta_persist_reboot",
      "disconnect_reconnect_led_patterns"
    ]
  },
  "integration_points": {
    "system_manager": ["state_change_notifications", "health"],
    "web_ui": ["network_settings_page", "scan_and_connect_dialog", "status_badges"],
    "mqtt": ["optional_status_and_events"],
    "led_manager": ["priority_overrides_for_tamper_and_wifi"]
  },
  "files_and_locations": {
    "component": "components/opdi_net/",
    "headers": ["components/opdi_net/include/opdi_net.h"],
    "sources": [
      "components/opdi_net/opdi_net.c",
      "components/opdi_net/opdi_net_hosted.c"
    ],
    "kconfig": "components/opdi_net/Kconfig",
    "sdk_defaults": "sdkconfig.defaults",
    "rest_routes": "main/routes_net.c",
    "docs": "docs/networking.md"
  }
}
